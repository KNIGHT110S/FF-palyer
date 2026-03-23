#include "AudioFrameProcessor.h"

#include <QByteArray>
#include <QtGlobal>

namespace {
QString ffErr2Str(int err)
{
    char buf[256] = {0};
    av_strerror(err, buf, sizeof(buf));
    return QString::fromUtf8(buf);
}

bool resolveUsableChannelLayout(const AVChannelLayout& source,
                                int fallbackChannels,
                                AVChannelLayout* outLayout,
                                QString* errorMessage)
{
    if (!outLayout) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Audio channel layout output target is invalid.");
        }
        return false;
    }

    const int channelCount = source.nb_channels > 0
        ? source.nb_channels
        : (fallbackChannels > 0 ? fallbackChannels : 2);

    if (source.nb_channels > 0
        && source.order != AV_CHANNEL_ORDER_UNSPEC
        && av_channel_layout_check(&source)) {
        const int ret = av_channel_layout_copy(outLayout, &source);
        if (ret < 0) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("Failed to copy audio channel layout: %1")
                                    .arg(ffErr2Str(ret));
            }
            return false;
        }
        return true;
    }

    av_channel_layout_default(outLayout, channelCount);
    if (!av_channel_layout_check(outLayout)) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Failed to derive a valid default audio channel layout.");
        }
        av_channel_layout_uninit(outLayout);
        return false;
    }

    return true;
}
}

AudioFrameProcessor::~AudioFrameProcessor()
{
    reset();
}

void AudioFrameProcessor::setOutputFormat(const QAudioFormat& format)
{
    m_outputSampleRate = format.sampleRate() > 0 ? format.sampleRate() : 44100;
    m_outputChannels = format.channelCount() > 0 ? format.channelCount() : 2;

    switch (format.sampleFormat()) {
    case QAudioFormat::UInt8:
        m_outputSampleFormat = AV_SAMPLE_FMT_U8;
        break;
    case QAudioFormat::Int16:
        m_outputSampleFormat = AV_SAMPLE_FMT_S16;
        break;
    case QAudioFormat::Int32:
        m_outputSampleFormat = AV_SAMPLE_FMT_S32;
        break;
    case QAudioFormat::Float:
        m_outputSampleFormat = AV_SAMPLE_FMT_FLT;
        break;
    case QAudioFormat::Unknown:
    default:
        m_outputSampleFormat = AV_SAMPLE_FMT_S16;
        break;
    }
}

bool AudioFrameProcessor::initialize(AVCodecContext* codecContext,
                                     AVRational timeBase,
                                     double playbackSpeed,
                                     QString* errorMessage)
{
    if (!codecContext) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Audio decoder context is invalid. Cannot initialize audio processor.");
        }
        return false;
    }

    if (!initSwrContext(codecContext, errorMessage)) {
        if (errorMessage) {
            if (errorMessage->isEmpty()) {
                *errorMessage = QStringLiteral("Failed to initialize audio output resampler.");
            }
        }
        return false;
    }

    if (shouldUseAudioFilter(playbackSpeed)) {
        if (!initAudioFilterGraph(codecContext, timeBase, playbackSpeed, errorMessage)) {
            if (errorMessage) {
                if (errorMessage->isEmpty()) {
                    *errorMessage = QStringLiteral("Failed to initialize audio speed filter graph.");
                }
            }
            return false;
        }
    } else {
        releaseAudioFilterGraph();
    }

    return true;
}

bool AudioFrameProcessor::resetAfterSeek(AVCodecContext* codecContext, QString* errorMessage)
{
    if (!codecContext) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Audio decoder context is invalid. Cannot reset audio processor after seek.");
        }
        return false;
    }

    if (!m_swrContext) {
        if (!initSwrContext(codecContext, errorMessage)) {
            if (errorMessage) {
                if (errorMessage->isEmpty()) {
                    *errorMessage = QStringLiteral("Failed to recreate audio output resampler after seek.");
                }
            }
            return false;
        }
        return true;
    }

    swr_close(m_swrContext);
    if (swr_init(m_swrContext) < 0) {
        swr_free(&m_swrContext);
        if (errorMessage) {
            *errorMessage = QStringLiteral("Failed to reset audio output resampler after seek.");
        }
        return false;
    }

    return true;
}

bool AudioFrameProcessor::rebuildForPlaybackSpeed(AVCodecContext* codecContext,
                                                  AVRational timeBase,
                                                  double playbackSpeed,
                                                  QString* errorMessage)
{
    if (!codecContext) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Audio decoder context is invalid. Cannot rebuild audio speed filter graph.");
        }
        return false;
    }

    if (shouldUseAudioFilter(playbackSpeed)) {
        if (!initAudioFilterGraph(codecContext, timeBase, playbackSpeed, errorMessage)) {
            if (errorMessage) {
                if (errorMessage->isEmpty()) {
                    *errorMessage = QStringLiteral("Failed to rebuild audio speed filter graph.");
                }
            }
            return false;
        }
    } else {
        releaseAudioFilterGraph();
    }

    return true;
}

bool AudioFrameProcessor::processDecodedFrame(AVFrame* frame,
                                              double fallbackPtsSec,
                                              std::vector<ProcessedAudioFrame>* outFrames,
                                              QString* errorMessage)
{
    if (!frame || !outFrames) {
        return false;
    }

    if (!m_filterEnabled) {
        std::vector<uint8_t> pcmData;
        if (!resampleAudioFrame(frame, pcmData)) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("Audio resampling failed.");
            }
            return false;
        }
        outFrames->push_back({std::move(pcmData), fallbackPtsSec});
        return true;
    }

    if (!m_audioBufferSrcContext) {
        return false;
    }

    const int ret = av_buffersrc_write_frame(m_audioBufferSrcContext, frame);
    if (ret < 0) {
        if (errorMessage) {
            char buf[256] = {0};
            av_strerror(ret, buf, sizeof(buf));
            *errorMessage = QStringLiteral("Audio filter input failed: %1")
                                .arg(QString::fromUtf8(buf));
        }
        return false;
    }

    return drainFilteredAudioFrames(outFrames, fallbackPtsSec, errorMessage);
}

bool AudioFrameProcessor::flush(std::vector<ProcessedAudioFrame>* outFrames, QString* errorMessage)
{
    if (!outFrames) {
        return false;
    }

    if (!m_filterEnabled) {
        Q_UNUSED(errorMessage);
        return true;
    }

    if (!m_audioBufferSrcContext) {
        return false;
    }

    av_buffersrc_close(m_audioBufferSrcContext, AV_NOPTS_VALUE, 0);
    return drainFilteredAudioFrames(outFrames, 0.0, errorMessage);
}

bool AudioFrameProcessor::isReady() const
{
    return m_swrContext && (!m_filterEnabled || (m_audioBufferSrcContext && m_audioBufferSinkContext));
}

void AudioFrameProcessor::reset()
{
    if (m_swrContext) {
        swr_free(&m_swrContext);
        m_swrContext = nullptr;
    }

    releaseAudioFilterGraph();
}

bool AudioFrameProcessor::initSwrContext(AVCodecContext* codecContext, QString* errorMessage)
{
    if (!codecContext) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Audio decoder context is invalid while initializing resampler.");
        }
        return false;
    }

    if (m_swrContext) {
        swr_free(&m_swrContext);
        m_swrContext = nullptr;
    }

    AVChannelLayout inputLayout{};
    if (!resolveUsableChannelLayout(codecContext->ch_layout, 2, &inputLayout, errorMessage)) {
        return false;
    }

    AVChannelLayout outputLayout{};
    av_channel_layout_default(&outputLayout, m_outputChannels > 0 ? m_outputChannels : 2);
    const int ret = swr_alloc_set_opts2(&m_swrContext,
                                        &outputLayout,
                                        m_outputSampleFormat,
                                        m_outputSampleRate,
                                        &inputLayout,
                                        codecContext->sample_fmt,
                                        codecContext->sample_rate,
                                        0,
                                        nullptr);
    av_channel_layout_uninit(&outputLayout);
    av_channel_layout_uninit(&inputLayout);
    if (ret < 0 || !m_swrContext) {
        if (errorMessage) {
            *errorMessage = ret < 0
                ? QStringLiteral("Failed to allocate audio resampler: %1").arg(ffErr2Str(ret))
                : QStringLiteral("Failed to allocate audio resampler.");
        }
        return false;
    }

    if (swr_init(m_swrContext) < 0) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Failed to initialize audio resampler.");
        }
        swr_free(&m_swrContext);
        return false;
    }

    return true;
}

bool AudioFrameProcessor::shouldUseAudioFilter(double playbackSpeed) const
{
    return qAbs(playbackSpeed - 1.0) > 0.001;
}

bool AudioFrameProcessor::initAudioFilterGraph(AVCodecContext* codecContext,
                                               AVRational timeBase,
                                               double playbackSpeed,
                                               QString* errorMessage)
{
    if (!codecContext) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Audio decoder context is invalid while building filter graph.");
        }
        return false;
    }

    releaseAudioFilterGraph();
    m_filterEnabled = false;

    m_audioFilterGraph = avfilter_graph_alloc();
    if (!m_audioFilterGraph) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Failed to allocate audio filter graph.");
        }
        return false;
    }

    const AVFilter* abuffer = avfilter_get_by_name("abuffer");
    const AVFilter* atempo = avfilter_get_by_name("atempo");
    const AVFilter* abuffersink = avfilter_get_by_name("abuffersink");
    if (!abuffer || !atempo || !abuffersink) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Required audio filters are missing. abuffer=%1, atempo=%2, abuffersink=%3")
                                .arg(abuffer ? QStringLiteral("ok") : QStringLiteral("missing"))
                                .arg(atempo ? QStringLiteral("ok") : QStringLiteral("missing"))
                                .arg(abuffersink ? QStringLiteral("ok") : QStringLiteral("missing"));
        }
        releaseAudioFilterGraph();
        return false;
    }

    int ret = 0;
    AVChannelLayout inputLayout{};
    if (!resolveUsableChannelLayout(codecContext->ch_layout, 2, &inputLayout, errorMessage)) {
        av_channel_layout_uninit(&inputLayout);
        releaseAudioFilterGraph();
        return false;
    }

    const AVRational safeTimeBase =
        (timeBase.num > 0 && timeBase.den > 0) ? timeBase : AVRational{1, codecContext->sample_rate};
    ret = avfilter_graph_create_filter(&m_audioBufferSrcContext, abuffer, "audio_in",
                                       nullptr, nullptr, m_audioFilterGraph);
    if (ret < 0) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Failed to create abuffer source filter: %1")
                                .arg(ffErr2Str(ret));
        }
        av_channel_layout_uninit(&inputLayout);
        releaseAudioFilterGraph();
        return false;
    }

    AVBufferSrcParameters* bufferParams = av_buffersrc_parameters_alloc();
    if (!bufferParams) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Failed to allocate abuffer source parameters.");
        }
        av_channel_layout_uninit(&inputLayout);
        releaseAudioFilterGraph();
        return false;
    }

    bufferParams->format = codecContext->sample_fmt;
    bufferParams->sample_rate = codecContext->sample_rate;
    bufferParams->time_base = safeTimeBase;
    ret = av_channel_layout_copy(&bufferParams->ch_layout, &inputLayout);
    if (ret < 0) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Failed to copy abuffer channel layout: %1")
                                .arg(ffErr2Str(ret));
        }
        av_free(bufferParams);
        av_channel_layout_uninit(&inputLayout);
        releaseAudioFilterGraph();
        return false;
    }

    ret = av_buffersrc_parameters_set(m_audioBufferSrcContext, bufferParams);
    av_channel_layout_uninit(&bufferParams->ch_layout);
    av_free(bufferParams);
    av_channel_layout_uninit(&inputLayout);
    if (ret < 0) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Failed to configure abuffer source filter: %1")
                                .arg(ffErr2Str(ret));
        }
        releaseAudioFilterGraph();
        return false;
    }

    const QByteArray tempoArgs =
        QByteArray("tempo=") + QByteArray::number(playbackSpeed, 'f', 3);
    ret = avfilter_graph_create_filter(&m_audioTempoContext, atempo, "audio_tempo",
                                       tempoArgs.constData(), nullptr, m_audioFilterGraph);
    if (ret < 0) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Failed to create atempo filter (%1): %2")
                                .arg(QString::fromUtf8(tempoArgs))
                                .arg(ffErr2Str(ret));
        }
        releaseAudioFilterGraph();
        return false;
    }

    ret = avfilter_graph_create_filter(&m_audioBufferSinkContext, abuffersink, "audio_out",
                                       nullptr, nullptr, m_audioFilterGraph);
    if (ret < 0) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Failed to create abuffersink filter: %1")
                                .arg(ffErr2Str(ret));
        }
        releaseAudioFilterGraph();
        return false;
    }

    ret = avfilter_link(m_audioBufferSrcContext, 0, m_audioTempoContext, 0);
    if (ret >= 0) {
        ret = avfilter_link(m_audioTempoContext, 0, m_audioBufferSinkContext, 0);
    }
    if (ret < 0) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Failed to link audio filter graph: %1")
                                .arg(ffErr2Str(ret));
        }
        releaseAudioFilterGraph();
        return false;
    }

    ret = avfilter_graph_config(m_audioFilterGraph, nullptr);
    if (ret < 0) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Failed to configure audio filter graph: %1")
                                .arg(ffErr2Str(ret));
        }
        releaseAudioFilterGraph();
        return false;
    }

    m_filterEnabled = true;
    return true;
}

bool AudioFrameProcessor::resampleAudioFrame(const AVFrame* frame, std::vector<uint8_t>& outPcm) const
{
    if (!m_swrContext || !frame || frame->nb_samples <= 0 || !frame->extended_data) {
        return false;
    }

    const int inputSampleRate = frame->sample_rate;
    if (inputSampleRate <= 0) {
        return false;
    }

    const int maxOutputSamples =
        av_rescale_rnd(swr_get_delay(m_swrContext, inputSampleRate) + frame->nb_samples,
                       m_outputSampleRate,
                       inputSampleRate,
                       AV_ROUND_UP);
    if (maxOutputSamples <= 0) {
        return false;
    }

    const int outputBytesPerSample = av_get_bytes_per_sample(m_outputSampleFormat);
    if (outputBytesPerSample <= 0) {
        return false;
    }

    const int outputBufferSize = maxOutputSamples * m_outputChannels * outputBytesPerSample;
    if (outputBufferSize <= 0) {
        return false;
    }

    outPcm.resize(outputBufferSize);
    uint8_t* outputData[1] = {outPcm.data()};

    const int convertedSamples = swr_convert(m_swrContext,
                                             outputData,
                                             maxOutputSamples,
                                             (const uint8_t**)frame->extended_data,
                                             frame->nb_samples);
    if (convertedSamples < 0) {
        return false;
    }

    outPcm.resize(convertedSamples * m_outputChannels * outputBytesPerSample);
    return true;
}

bool AudioFrameProcessor::drainFilteredAudioFrames(std::vector<ProcessedAudioFrame>* outFrames,
                                                   double fallbackPtsSec,
                                                   QString* errorMessage)
{
    if (!outFrames || !m_audioBufferSinkContext) {
        return false;
    }

    AVFrame* filteredFrame = av_frame_alloc();
    if (!filteredFrame) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Failed to allocate filtered audio output buffer.");
        }
        return false;
    }

    bool success = true;
    while (true) {
        const int ret = av_buffersink_get_frame(m_audioBufferSinkContext, filteredFrame);
        if (ret == 0) {
            std::vector<uint8_t> pcmData;
            if (resampleAudioFrame(filteredFrame, pcmData)) {
                double ptsSec = filteredAudioPtsToSec(filteredFrame);
                if (ptsSec <= 0.0) {
                    ptsSec = fallbackPtsSec;
                }
                outFrames->push_back({std::move(pcmData), ptsSec});
            }
            av_frame_unref(filteredFrame);
            continue;
        }

        if (ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) {
            if (errorMessage) {
                char buf[256] = {0};
                av_strerror(ret, buf, sizeof(buf));
                *errorMessage = QStringLiteral("Audio filter output failed: %1")
                                    .arg(QString::fromUtf8(buf));
            }
            success = false;
        }
        break;
    }

    av_frame_free(&filteredFrame);
    return success;
}

double AudioFrameProcessor::filteredAudioPtsToSec(const AVFrame* frame) const
{
    if (!frame || frame->pts == AV_NOPTS_VALUE || !m_audioBufferSinkContext) {
        return 0.0;
    }

    const AVRational timeBase = av_buffersink_get_time_base(m_audioBufferSinkContext);
    if (timeBase.num <= 0 || timeBase.den <= 0) {
        return 0.0;
    }

    return av_q2d(timeBase) * frame->pts;
}

void AudioFrameProcessor::releaseAudioFilterGraph()
{
    m_filterEnabled = false;
    m_audioBufferSrcContext = nullptr;
    m_audioTempoContext = nullptr;
    m_audioBufferSinkContext = nullptr;

    if (m_audioFilterGraph) {
        avfilter_graph_free(&m_audioFilterGraph);
        m_audioFilterGraph = nullptr;
    }
}
