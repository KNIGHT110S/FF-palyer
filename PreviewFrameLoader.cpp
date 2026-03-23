#include "PreviewFrameLoader.h"

PreviewFrameLoader::PreviewFrameLoader(QObject* parent)
    : QObject(parent)
{
}

PreviewFrameLoader::~PreviewFrameLoader()
{
    close();
}

QString PreviewFrameLoader::ffErr2Str(int err)
{
    char buf[256] = {0};
    av_strerror(err, buf, sizeof(buf));
    return QString::fromUtf8(buf);
}

bool PreviewFrameLoader::openMedia(const QString& path)
{
    close();

    AVFormatContext* formatContext = nullptr;
    int ret = avformat_open_input(&formatContext, path.toUtf8().constData(), nullptr, nullptr);
    if (ret < 0) {
        emit errorOccurred(QStringLiteral("Failed to open preview media source: %1").arg(ffErr2Str(ret)));
        return false;
    }

    ret = avformat_find_stream_info(formatContext, nullptr);
    if (ret < 0) {
        emit errorOccurred(QStringLiteral("Failed to read preview media information: %1").arg(ffErr2Str(ret)));
        avformat_close_input(&formatContext);
        return false;
    }

    int videoStreamIndex = -1;
    for (unsigned int i = 0; i < formatContext->nb_streams; ++i) {
        if (formatContext->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            videoStreamIndex = static_cast<int>(i);
            break;
        }
    }

    if (videoStreamIndex < 0) {
        emit errorOccurred(QStringLiteral("Preview loader could not find a video stream."));
        avformat_close_input(&formatContext);
        return false;
    }

    AVStream* videoStream = formatContext->streams[videoStreamIndex];
    const AVCodec* codec = avcodec_find_decoder(videoStream->codecpar->codec_id);
    if (!codec) {
        emit errorOccurred(QStringLiteral("Preview loader could not find a matching video decoder."));
        avformat_close_input(&formatContext);
        return false;
    }

    AVCodecContext* videoCodecContext = avcodec_alloc_context3(codec);
    if (!videoCodecContext) {
        emit errorOccurred(QStringLiteral("Failed to allocate preview decoder context."));
        avformat_close_input(&formatContext);
        return false;
    }

    ret = avcodec_parameters_to_context(videoCodecContext, videoStream->codecpar);
    if (ret < 0) {
        emit errorOccurred(QStringLiteral("Failed to copy preview decoder parameters: %1").arg(ffErr2Str(ret)));
        avcodec_free_context(&videoCodecContext);
        avformat_close_input(&formatContext);
        return false;
    }

    ret = avcodec_open2(videoCodecContext, codec, nullptr);
    if (ret < 0) {
        emit errorOccurred(QStringLiteral("Failed to open preview decoder: %1").arg(ffErr2Str(ret)));
        avcodec_free_context(&videoCodecContext);
        avformat_close_input(&formatContext);
        return false;
    }

    QString errorMessage;
    if (!m_frameConverter.initialize(videoCodecContext, &errorMessage)) {
        emit errorOccurred(errorMessage);
        avcodec_free_context(&videoCodecContext);
        avformat_close_input(&formatContext);
        return false;
    }

    m_formatContext = formatContext;
    m_videoCodecContext = videoCodecContext;
    m_videoStreamIndex = videoStreamIndex;
    return true;
}

void PreviewFrameLoader::close()
{
    stopPreviewWorker();
    m_frameConverter.reset();

    if (m_videoCodecContext) {
        avcodec_free_context(&m_videoCodecContext);
        m_videoCodecContext = nullptr;
    }

    if (m_formatContext) {
        avformat_close_input(&m_formatContext);
        m_formatContext = nullptr;
    }

    m_videoStreamIndex = -1;
}

bool PreviewFrameLoader::loadPreviewFrame(qint64 targetMs)
{
    stopPreviewWorker();

    QImage image;
    qint64 ptsMs = targetMs;
    if (!loadPreviewFrameInternal(targetMs, &image, &ptsMs)) {
        return false;
    }

    emit previewImageReady(image, ptsMs);
    return true;
}

void PreviewFrameLoader::requestPreviewFrame(qint64 targetMs)
{
    if (!m_formatContext || !m_videoCodecContext || m_videoStreamIndex < 0) {
        return;
    }

    if (targetMs < 0) {
        targetMs = 0;
    }

    {
        std::lock_guard<std::mutex> lock(m_previewMutex);
        if (!m_previewThread.joinable()) {
            m_stopRequested = false;
            m_requestPending = false;
            m_requestMs = -1;
            m_previewThread = std::thread(&PreviewFrameLoader::previewLoop, this);
        }
        m_requestMs = targetMs;
        m_requestPending = true;
        ++m_requestSerial;
    }

    m_previewCondition.notify_one();
}

void PreviewFrameLoader::cancelPendingPreview()
{
    stopPreviewWorker();
}

qint64 PreviewFrameLoader::framePtsToMs(const AVFrame* frame) const
{
    if (!m_formatContext || m_videoStreamIndex < 0) {
        return 0;
    }

    const AVStream* stream = m_formatContext->streams[m_videoStreamIndex];
    int64_t pts = frame->best_effort_timestamp;
    if (pts == AV_NOPTS_VALUE) {
        pts = frame->pts;
    }
    if (pts == AV_NOPTS_VALUE) {
        return 0;
    }

    return av_rescale_q(pts, stream->time_base, AVRational{1, 1000});
}

bool PreviewFrameLoader::loadPreviewFrameInternal(qint64 targetMs, QImage* outImage, qint64* outPtsMs)
{
    if (!m_formatContext || !m_videoCodecContext || m_videoStreamIndex < 0 || !outImage || !outPtsMs) {
        emit errorOccurred(QStringLiteral("Preview loader is not ready."));
        return false;
    }

    if (targetMs < 0) {
        targetMs = 0;
    }

    AVStream* stream = m_formatContext->streams[m_videoStreamIndex];
    const int64_t targetPts = av_rescale_q(targetMs, AVRational{1, 1000}, stream->time_base);
    int ret = av_seek_frame(m_formatContext, m_videoStreamIndex, targetPts, AVSEEK_FLAG_BACKWARD);
    if (ret < 0) {
        emit errorOccurred(QStringLiteral("Preview seek failed: %1").arg(ffErr2Str(ret)));
        return false;
    }

    avcodec_flush_buffers(m_videoCodecContext);

    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    if (!packet || !frame) {
        if (packet) {
            av_packet_free(&packet);
        }
        if (frame) {
            av_frame_free(&frame);
        }
        emit errorOccurred(QStringLiteral("Failed to allocate preview buffers."));
        return false;
    }

    m_videoCodecContext->skip_frame = AVDISCARD_NONREF;
    m_videoCodecContext->skip_loop_filter = AVDISCARD_ALL;
    m_videoCodecContext->skip_idct = AVDISCARD_ALL;

    bool previewReady = false;
    int maxPackets = 60;

    while (!previewReady && maxPackets-- > 0 && (ret = av_read_frame(m_formatContext, packet)) >= 0) {
        if (packet->stream_index != m_videoStreamIndex) {
            av_packet_unref(packet);
            continue;
        }

        ret = avcodec_send_packet(m_videoCodecContext, packet);
        av_packet_unref(packet);
        if (ret < 0 && ret != AVERROR(EAGAIN)) {
            emit errorOccurred(QStringLiteral("Failed to send preview packet: %1").arg(ffErr2Str(ret)));
            break;
        }

        while (!previewReady) {
            ret = avcodec_receive_frame(m_videoCodecContext, frame);
            if (ret == 0) {
                const qint64 ptsMs = framePtsToMs(frame);
                if (ptsMs < targetMs) {
                    av_frame_unref(frame);
                    continue;
                }

                if (m_frameConverter.convert(frame, outImage)) {
                    *outPtsMs = ptsMs;
                    previewReady = true;
                }
                av_frame_unref(frame);
                continue;
            }

            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                break;
            }

            emit errorOccurred(QStringLiteral("Failed to read preview frame: %1").arg(ffErr2Str(ret)));
            break;
        }
    }

    m_videoCodecContext->skip_frame = AVDISCARD_DEFAULT;
    m_videoCodecContext->skip_loop_filter = AVDISCARD_DEFAULT;
    m_videoCodecContext->skip_idct = AVDISCARD_DEFAULT;

    av_frame_free(&frame);
    av_packet_free(&packet);
    return previewReady;
}

void PreviewFrameLoader::previewLoop()
{
    while (true) {
        qint64 targetMs = -1;
        uint64_t requestSerial = 0;

        {
            std::unique_lock<std::mutex> lock(m_previewMutex);
            m_previewCondition.wait(lock, [this]() {
                return m_stopRequested || m_requestPending;
            });
            if (m_stopRequested) {
                break;
            }

            targetMs = m_requestMs;
            requestSerial = m_requestSerial;
            m_requestPending = false;
        }

        QImage image;
        qint64 ptsMs = targetMs;
        const bool success = loadPreviewFrameInternal(targetMs, &image, &ptsMs);

        bool shouldEmit = false;
        {
            std::lock_guard<std::mutex> lock(m_previewMutex);
            shouldEmit = success
                && !m_stopRequested
                && requestSerial == m_requestSerial
                && !m_requestPending;
        }

        if (shouldEmit) {
            emit previewImageReady(image, ptsMs);
        }
    }
}

void PreviewFrameLoader::stopPreviewWorker()
{
    {
        std::lock_guard<std::mutex> lock(m_previewMutex);
        m_stopRequested = true;
        m_requestPending = false;
        m_requestMs = -1;
    }

    m_previewCondition.notify_all();

    if (m_previewThread.joinable()) {
        m_previewThread.join();
    }

    {
        std::lock_guard<std::mutex> lock(m_previewMutex);
        m_stopRequested = false;
    }
}
