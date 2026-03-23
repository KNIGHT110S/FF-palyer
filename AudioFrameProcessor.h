#ifndef AUDIOFRAMEPROCESSOR_H
#define AUDIOFRAMEPROCESSOR_H

#include <QAudioFormat>
#include <QString>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}

struct ProcessedAudioFrame {
    std::vector<uint8_t> pcmData;
    double ptsSec = 0.0;
};

class AudioFrameProcessor
{
public:
    AudioFrameProcessor() = default;
    ~AudioFrameProcessor();

    void setOutputFormat(const QAudioFormat& format);
    bool initialize(AVCodecContext* codecContext,
                    AVRational timeBase,
                    double playbackSpeed,
                    QString* errorMessage);
    bool resetAfterSeek(AVCodecContext* codecContext, QString* errorMessage);
    bool rebuildForPlaybackSpeed(AVCodecContext* codecContext,
                                 AVRational timeBase,
                                 double playbackSpeed,
                                 QString* errorMessage);
    bool processDecodedFrame(AVFrame* frame,
                             double fallbackPtsSec,
                             std::vector<ProcessedAudioFrame>* outFrames,
                             QString* errorMessage);
    bool flush(std::vector<ProcessedAudioFrame>* outFrames, QString* errorMessage);
    bool isReady() const;
    void reset();

private:
    bool shouldUseAudioFilter(double playbackSpeed) const;
    bool initSwrContext(AVCodecContext* codecContext, QString* errorMessage);
    bool initAudioFilterGraph(AVCodecContext* codecContext,
                              AVRational timeBase,
                              double playbackSpeed,
                              QString* errorMessage);
    bool resampleAudioFrame(const AVFrame* frame, std::vector<uint8_t>& outPcm) const;
    bool drainFilteredAudioFrames(std::vector<ProcessedAudioFrame>* outFrames,
                                  double fallbackPtsSec,
                                  QString* errorMessage);
    double filteredAudioPtsToSec(const AVFrame* frame) const;
    void releaseAudioFilterGraph();

    int m_outputSampleRate = 44100;
    int m_outputChannels = 2;
    AVSampleFormat m_outputSampleFormat = AV_SAMPLE_FMT_S16;
    SwrContext* m_swrContext = nullptr;
    AVFilterGraph* m_audioFilterGraph = nullptr;
    AVFilterContext* m_audioBufferSrcContext = nullptr;
    AVFilterContext* m_audioTempoContext = nullptr;
    AVFilterContext* m_audioBufferSinkContext = nullptr;
    bool m_filterEnabled = false;
};

#endif // AUDIOFRAMEPROCESSOR_H
