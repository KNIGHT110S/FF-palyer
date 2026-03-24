#include "VideoDecoder.h"

#include <QByteArray>
#include <memory>
#include <thread>

VideoDecoder::VideoDecoder(QObject* parent)
    : QObject(parent)
{
}

VideoDecoder::~VideoDecoder()
{
    close();
}

QString VideoDecoder::ffErr2Str(int err)
{
    char buf[256] = {0};
    av_strerror(err, buf, sizeof(buf));
    return QString::fromUtf8(buf);
}

double VideoDecoder::streamFps(AVStream* st)
{
    AVRational r = st->avg_frame_rate.num ? st->avg_frame_rate : st->r_frame_rate;
    if (r.num == 0 || r.den == 0) {
        return 0.0;
    }
    return av_q2d(r);
}

bool VideoDecoder::open(const QString& path)
{
    close();

    AVFormatContext* ctx = nullptr;
    int ret = avformat_open_input(&ctx, path.toUtf8().constData(), nullptr, nullptr);
    if (ret < 0) {
        emit errorOccurred(QStringLiteral("Failed to open media file: %1").arg(ffErr2Str(ret)));
        return false;
    }

    ret = avformat_find_stream_info(ctx, nullptr);
    if (ret < 0) {
        emit errorOccurred(QStringLiteral("Failed to read media information: %1").arg(ffErr2Str(ret)));
        avformat_close_input(&ctx);
        return false;
    }

    // Reset speed on new file open to default or keep user pref?
    // User usually expects speed to persist or reset. Let's reset for safety.
    // playbackSpeed.store(1.0); 
    seekTargetPtsMs.store(-1);
    int vIndex = -1;
    int aIndex = -1;
    for (unsigned int i = 0; i < ctx->nb_streams; ++i) {
        AVStream* st = ctx->streams[i];
        if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && vIndex < 0) {
            vIndex = static_cast<int>(i);
        } else if (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && aIndex < 0) {
            aIndex = static_cast<int>(i);
        }
    }

    if (vIndex < 0) {
        emit errorOccurred(QStringLiteral("No video stream found. Only video files are supported."));
        avformat_close_input(&ctx);
        return false;
    }

    fmtCtx = ctx;
    videoStreamIndex = vIndex;
    audioStreamIndex = aIndex;
    AVStream* vStream = fmtCtx->streams[videoStreamIndex];
    AVCodecParameters* vPar = vStream->codecpar;

    const AVCodec* decoder = avcodec_find_decoder(vPar->codec_id);
    if (!decoder) {
        emit errorOccurred(QStringLiteral("No available video decoder found."));
        close();
        return false;
    }

    videoCodecCtx = avcodec_alloc_context3(decoder);
    if (!videoCodecCtx) {
        emit errorOccurred(QStringLiteral("Failed to allocate video decoder context."));
        close();
        return false;
    }

    ret = avcodec_parameters_to_context(videoCodecCtx, vPar);
    if (ret < 0) {
        emit errorOccurred(QStringLiteral("Failed to copy video decoder parameters: %1").arg(ffErr2Str(ret)));
        close();
        return false;
    }

    ret = avcodec_open2(videoCodecCtx, decoder, nullptr);
    if (ret < 0) {
        emit errorOccurred(QStringLiteral("Failed to open video decoder: %1").arg(ffErr2Str(ret)));
        close();
        return false;
    }
    QString errorMessage;
    if (!m_videoFrameConverter.initialize(videoCodecCtx, &errorMessage)) {
        emit errorOccurred(errorMessage);
        close();
        return false;
    }

    if (audioStreamIndex >= 0) {
        AVStream* aStream = fmtCtx->streams[audioStreamIndex];
        AVCodecParameters* aPar = aStream->codecpar;
        
        const AVCodec* aDecoder = avcodec_find_decoder(aPar->codec_id);
        if (aDecoder) {
            audioCodecCtx = avcodec_alloc_context3(aDecoder);
            if (audioCodecCtx) {
                // Copy parameters but verify sample rate / channel layout on codec context
                if (avcodec_parameters_to_context(audioCodecCtx, aPar) >= 0) {
                    // Ensure sample rate is valid
                    if (audioCodecCtx->sample_rate <= 0) {
                        audioCodecCtx->sample_rate = 44100;
                    }
                    if (audioCodecCtx->ch_layout.nb_channels <= 0) {
                        av_channel_layout_default(&audioCodecCtx->ch_layout, 2);
                    }

                    if (avcodec_open2(audioCodecCtx, aDecoder, nullptr) < 0) {
                        avcodec_free_context(&audioCodecCtx);
                        audioCodecCtx = nullptr;
                        audioStreamIndex = -1;
                    }
                } else {
                     avcodec_free_context(&audioCodecCtx);
                     audioCodecCtx = nullptr;
                     audioStreamIndex = -1;
                }
            }
        } else {
            audioStreamIndex = -1;
        }
    }

    MediaInfo info;

    if (fmtCtx->duration > 0) {
        info.durationMs = fmtCtx->duration / (AV_TIME_BASE / 1000);
    }
    info.width = vPar->width;
    info.height = vPar->height;
    info.fps = streamFps(vStream);

    if (audioStreamIndex >= 0 && audioCodecCtx) {
        AVStream* aStream = fmtCtx->streams[audioStreamIndex];
        AVCodecParameters* aPar = aStream->codecpar;
        info.hasAudio = true;
        info.sampleRate = audioCodecCtx->sample_rate > 0 ? audioCodecCtx->sample_rate : aPar->sample_rate;
        audioChainValid.store(false);
        audioSpeedChanged.store(false);
    } else {
        audioChainValid.store(false);
        audioSpeedChanged.store(false);
    }

    emit mediaInfoReady(info);
    return true;
}

bool VideoDecoder::startDecoding()
{
    if (!fmtCtx || !videoCodecCtx || videoStreamIndex < 0) {
        emit errorOccurred(QStringLiteral("Decoder is not initialized. Cannot start decoding."));
        return false;
    }
    if (decoding.load()) {
        return true;
    }
    if (decodeThread.joinable()) {
        decodeThread.join();
    }

    abortDecode.store(false);
    m_decodePacer.reset();
    const qint64 seekFloor = seekTargetPtsMs.load();
    m_decodePacer.setSeekFloor(seekFloor >= 0 ? seekFloor : 0);
    decoding.store(true);
    decodeThread = std::thread(&VideoDecoder::decodeLoop, this);
    return true;
}

void VideoDecoder::stopDecoding()
{
    abortDecode.store(true);
    if (decodeThread.joinable()) {
        decodeThread.join();
    }
    decoding.store(false);
}

bool VideoDecoder::seekMs(qint64 targetMs)
{
    if (!fmtCtx || !videoCodecCtx || videoStreamIndex < 0) {
        emit errorOccurred(QStringLiteral("Decoder is not ready. Cannot perform seek."));
        return false;
    }

    stopDecoding();

    if (targetMs < 0) {
        targetMs = 0;
    }
    seekTargetPtsMs.store(targetMs);
    m_decodePacer.setSeekFloor(targetMs);

    AVStream* stream = fmtCtx->streams[videoStreamIndex];
    const int64_t targetPts = av_rescale_q(targetMs, AVRational{1, 1000}, stream->time_base);
    const int ret = av_seek_frame(fmtCtx, videoStreamIndex, targetPts, AVSEEK_FLAG_BACKWARD);
    if (ret < 0) {
        emit errorOccurred(QStringLiteral("Seek failed: %1").arg(ffErr2Str(ret)));
        return false;
    }

    avcodec_flush_buffers(videoCodecCtx);
    if (audioCodecCtx) {
        avcodec_flush_buffers(audioCodecCtx);
    }
    QString errorMessage;
    if (audioCodecCtx && !m_audioFrameProcessor.resetAfterSeek(audioCodecCtx, &errorMessage)) {
        audioChainValid.store(false);
        emit errorOccurred(errorMessage);
    }
    return true;
}

void VideoDecoder::setPlaybackSpeed(double speed)
{
    if (speed <= 0.0) return;
    playbackSpeed.store(speed);
    m_decodePacer.setPlaybackSpeed(speed);
    audioSpeedChanged.store(true);
}

bool VideoDecoder::setAudioOutputFormat(const QAudioFormat& format)
{
    m_audioFrameProcessor.setOutputFormat(format);

    if (!audioCodecCtx || audioStreamIndex < 0 || !fmtCtx) {
        audioChainValid.store(false);
        return false;
    }

    QString errorMessage;
    if (!m_audioFrameProcessor.initialize(audioCodecCtx,
                                          fmtCtx->streams[audioStreamIndex]->time_base,
                                          playbackSpeed.load(),
                                          &errorMessage)) {
        audioChainValid.store(false);
        emit errorOccurred(errorMessage);
        return false;
    }

    audioChainValid.store(true);
    return true;
}

void VideoDecoder::close()
{
    audioChainValid.store(false);
    stopDecoding();
    m_videoFrameConverter.reset();
    m_audioFrameProcessor.reset();

    if (videoCodecCtx) {
        avcodec_free_context(&videoCodecCtx);
        videoCodecCtx = nullptr;
    }
    
    if (audioCodecCtx) {
        avcodec_free_context(&audioCodecCtx);
        audioCodecCtx = nullptr;
    }

    if (fmtCtx) {
        avformat_close_input(&fmtCtx);
        fmtCtx = nullptr;
    }
    videoStreamIndex = -1;
    audioStreamIndex = -1;
}

qint64 VideoDecoder::framePtsToMs(const AVFrame* frame) const
{
    if (!fmtCtx || videoStreamIndex < 0)
        return 0;

    const AVStream* stream = fmtCtx->streams[videoStreamIndex];
    int64_t pts = frame->best_effort_timestamp;
    if (pts == AV_NOPTS_VALUE) {
        pts = frame->pts;
    }
    if (pts == AV_NOPTS_VALUE) {
        return 0;
    }
    return av_rescale_q(pts, stream->time_base, AVRational{1, 1000});
}

double VideoDecoder::framePtsToSec(const AVFrame* frame) const
{
    if (!fmtCtx || audioStreamIndex < 0) return 0.0;
    const AVStream* stream = fmtCtx->streams[audioStreamIndex];
    int64_t pts = frame->best_effort_timestamp;
    if (pts == AV_NOPTS_VALUE) pts = frame->pts;
    if (pts == AV_NOPTS_VALUE) return 0.0;
    return av_q2d(stream->time_base) * pts;
}

void VideoDecoder::drainFrames(AVFrame* frame, bool* fatalError, bool* eofReached, bool* seekTargetReached)
{
    if (seekTargetReached) {
        *seekTargetReached = false;
    }
    if (!videoCodecCtx) {
        return;
    }
    while (!abortDecode.load()) {
        int ret = avcodec_receive_frame(videoCodecCtx, frame);
        if (ret == 0) {
            const qint64 ptsMs = framePtsToMs(frame);

            // Fast-forward logic: skip frames until we reach the seek target
            // This prevents "freeze" effect when seeking backward to a keyframe
            // by avoiding expensive conversion and signaling for intermediate frames.
            const qint64 target = seekTargetPtsMs.load();
            if (target >= 0) {
                if (ptsMs < target) {
                    av_frame_unref(frame);
                    continue;
                }
                if (seekTargetReached) {
                    *seekTargetReached = true;
                }
            }

            m_decodePacer.pace(ptsMs, abortDecode);

            QImage image;
            if (m_videoFrameConverter.convert(frame, &image)) {
                emit videoImageReady(image, ptsMs);
            }

            av_frame_unref(frame);
            continue;
        }
        if (ret == AVERROR(EAGAIN)) {
            return;
        }
        if (ret == AVERROR_EOF) {
            *eofReached = true;
            return;
        }

        emit errorOccurred(QStringLiteral("Failed to receive decoded video frame: %1").arg(ffErr2Str(ret)));
        *fatalError = true;
        return;
    }
}

void VideoDecoder::drainAudioFrames(AVFrame* frame, bool* fatalError, bool* eofReached)
{
    Q_UNUSED(fatalError);

    if (!audioCodecCtx) {
        return;
    }
    while (!abortDecode.load()) {
        int ret = avcodec_receive_frame(audioCodecCtx, frame);
        if (ret == 0) {
            if (audioSpeedChanged.exchange(false)) {
                QString errorMessage;
                if (!m_audioFrameProcessor.rebuildForPlaybackSpeed(audioCodecCtx,
                                                                  fmtCtx->streams[audioStreamIndex]->time_base,
                                                                  playbackSpeed.load(),
                                                                  &errorMessage)) {
                    audioChainValid.store(false);
                    emit errorOccurred(errorMessage);
                } else {
                    audioChainValid.store(true);
                }
            }

            if (audioChainValid.load() && m_audioFrameProcessor.isReady()) {
                const double fallbackPtsSec = framePtsToSec(frame);
                std::vector<ProcessedAudioFrame> processedFrames;
                QString errorMessage;
                if (!m_audioFrameProcessor.processDecodedFrame(frame,
                                                               fallbackPtsSec,
                                                               &processedFrames,
                                                               &errorMessage)) {
                    audioChainValid.store(false);
                    emit errorOccurred(errorMessage);
                } else {
                    for (const ProcessedAudioFrame& processedFrame : processedFrames) {
                        emit audioFrameDecoded(processedFrame.pcmData, processedFrame.ptsSec);
                    }
                }
            }
            av_frame_unref(frame);
            continue;
        }
        if (ret == AVERROR(EAGAIN)) {
            return;
        }
        if (ret == AVERROR_EOF) {
            *eofReached = true;
            return;
        }
        return;
    }
}

void VideoDecoder::decodeLoop()
{
    struct AVPacketDeleter {
        void operator()(AVPacket* packet) const
        {
            av_packet_free(&packet);
        }
    };
    struct AVFrameDeleter {
        void operator()(AVFrame* frame) const
        {
            av_frame_free(&frame);
        }
    };

    std::unique_ptr<AVPacket, AVPacketDeleter> packet(av_packet_alloc());
    std::unique_ptr<AVFrame, AVFrameDeleter> frame(av_frame_alloc());

    if (!packet || !frame) {
        emit errorOccurred(QStringLiteral("Failed to allocate decoder buffers."));
        decoding.store(false);
        return;
    }

    bool fatalError = false;
    bool eofReached = false;
    bool videoSkipPolicyInitialized = false;
    bool fastSeekDiscardEnabled = false;
    // Audio EOF separate tracking? For simplicity, we mostly care about video EOF for now or master clock.
    // But let's track both if we want perfect ending.

    auto applyVideoSkipPolicy = [&](bool enableFastSeekDiscard) {
        if (!videoCodecCtx) {
            return;
        }
        if (videoSkipPolicyInitialized && fastSeekDiscardEnabled == enableFastSeekDiscard) {
            return;
        }

        videoSkipPolicyInitialized = true;
        fastSeekDiscardEnabled = enableFastSeekDiscard;
        videoCodecCtx->skip_frame = enableFastSeekDiscard ? AVDISCARD_NONREF : AVDISCARD_DEFAULT;
        videoCodecCtx->skip_loop_filter = enableFastSeekDiscard ? AVDISCARD_ALL : AVDISCARD_DEFAULT;
        videoCodecCtx->skip_idct = enableFastSeekDiscard ? AVDISCARD_ALL : AVDISCARD_DEFAULT;
    };

    auto completePendingSeek = [&](bool seekTargetReached) {
        if (!seekTargetReached) {
            return;
        }
        seekTargetPtsMs.store(-1);
        applyVideoSkipPolicy(false);
    };

    applyVideoSkipPolicy(seekTargetPtsMs.load() >= 0);

    auto sendVideoPacket = [&](AVPacket* pendingPacket) -> bool {
        while (!abortDecode.load()) {
            const int sendRet = avcodec_send_packet(videoCodecCtx, pendingPacket);
            if (sendRet == AVERROR(EAGAIN)) {
                bool seekTargetReached = false;
                drainFrames(frame.get(), &fatalError, &eofReached, &seekTargetReached);
                completePendingSeek(seekTargetReached);
                if (fatalError || eofReached || abortDecode.load()) {
                    return false;
                }
                continue;
            }
            if (sendRet < 0) {
                emit errorOccurred(QStringLiteral("Failed to send video packet: %1").arg(ffErr2Str(sendRet)));
                fatalError = true;
                return false;
            }
            return true;
        }
        return false;
    };

    auto sendAudioPacket = [&](AVPacket* pendingPacket) -> bool {
        while (!abortDecode.load()) {
            const int sendRet = avcodec_send_packet(audioCodecCtx, pendingPacket);
            if (sendRet == AVERROR(EAGAIN)) {
                drainAudioFrames(frame.get(), &fatalError, &eofReached);
                if (fatalError || eofReached || abortDecode.load()) {
                    return false;
                }
                continue;
            }
            if (sendRet < 0) {
                emit errorOccurred(QStringLiteral("Failed to send audio packet: %1").arg(ffErr2Str(sendRet)));
                audioChainValid.store(false);
                return false;
            }
            return true;
        }
        return false;
    };

    while (!abortDecode.load()) {
        const qint64 currentSeekTarget = seekTargetPtsMs.load();
        applyVideoSkipPolicy(currentSeekTarget >= 0);

        int ret = av_read_frame(fmtCtx, packet.get());
        if (ret == AVERROR_EOF) {
            avcodec_send_packet(videoCodecCtx, nullptr);
            if (audioCodecCtx) avcodec_send_packet(audioCodecCtx, nullptr);
            
            bool seekTargetReached = false;
            drainFrames(frame.get(), &fatalError, &eofReached, &seekTargetReached);
            completePendingSeek(seekTargetReached);
            if (audioCodecCtx) drainAudioFrames(frame.get(), &fatalError, &eofReached);
            if (audioChainValid.load() && m_audioFrameProcessor.isReady()) {
                std::vector<ProcessedAudioFrame> processedFrames;
                QString errorMessage;
                if (!m_audioFrameProcessor.flush(&processedFrames, &errorMessage)) {
                    audioChainValid.store(false);
                    emit errorOccurred(errorMessage);
                } else {
                    for (const ProcessedAudioFrame& processedFrame : processedFrames) {
                        emit audioFrameDecoded(processedFrame.pcmData, processedFrame.ptsSec);
                    }
                }
            }
            break;
        }
        if (ret < 0) {
            emit errorOccurred(QStringLiteral("Failed to read compressed packet: %1").arg(ffErr2Str(ret)));
            fatalError = true;
            break;
        }

        if (packet->stream_index == videoStreamIndex) {
            const bool sent = sendVideoPacket(packet.get());
            av_packet_unref(packet.get());

            if (!sent) {
                if (fatalError || eofReached || abortDecode.load()) {
                    break;
                }
                continue;
            }
            bool seekTargetReached = false;
            drainFrames(frame.get(), &fatalError, &eofReached, &seekTargetReached);
            completePendingSeek(seekTargetReached);
        } 
        else if (audioStreamIndex >= 0 && packet->stream_index == audioStreamIndex && audioCodecCtx) {
            const bool sent = sendAudioPacket(packet.get());
            av_packet_unref(packet.get());

            if (!sent) {
                if (fatalError || eofReached || abortDecode.load()) {
                    break;
                }
                continue;
            }
            drainAudioFrames(frame.get(), &fatalError, &eofReached);
        }
        else {
            av_packet_unref(packet.get());
        }

        if (fatalError) {
            break;
        }
    }

    applyVideoSkipPolicy(false);
    decoding.store(false);
    if (!fatalError && eofReached && !abortDecode.load()) {
        emit decodeFinished();
    }
}

