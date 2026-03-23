#ifndef VIDEODECODER_H
#define VIDEODECODER_H

#include <QObject>
#include <QAudioFormat>
#include <QImage>
#include <QString>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/avstring.h>
}

#include "AudioFrameProcessor.h"
#include "DecodePacer.h"
#include "VideoFrameConverter.h"

struct MediaInfo {
    qint64 durationMs = 0;
    int width = 0;
    int height = 0;
    double fps = 0.0;
    bool hasAudio = false;
    int sampleRate = 0;
};

class VideoDecoder : public QObject {
    Q_OBJECT
public:
    explicit VideoDecoder(QObject* parent = nullptr);
    ~VideoDecoder();
    bool open(const QString& path);
    bool startDecoding();
    void stopDecoding();
    bool seekMs(qint64 targetMs);
    bool loadPreviewFrame(qint64 targetMs);
    void requestPreviewFrame(qint64 targetMs);
    void cancelPendingPreview();
    void setPlaybackSpeed(double speed);
    bool setAudioOutputFormat(const QAudioFormat& format);
    void close();

signals:
    void mediaInfoReady(const MediaInfo& info);
    void videoFrameDecoded(qint64 ptsMs, int width, int height);
    void videoImageReady(const QImage& image, qint64 ptsMs);
    void previewImageReady(const QImage& image, qint64 ptsMs);
    void audioFrameDecoded(const std::vector<uint8_t>& pcmData, double ptsSec);
    void decodeFinished();
    void errorOccurred(const QString& message);

private:
    static QString ffErr2Str(int err);
    static double streamFps(AVStream* st);
    void decodeLoop();
    void drainFrames(AVFrame* frame, bool* fatalError, bool* eofReached);
    void drainAudioFrames(AVFrame* frame, bool* fatalError, bool* eofReached);
    qint64 framePtsToMs(const AVFrame* frame) const;
    double framePtsToSec(const AVFrame* frame) const;
    bool loadPreviewFrameInternal(qint64 targetMs, QImage* outImage, qint64* outPtsMs);
    void previewLoop();
    void stopPreviewWorker();

    AVFormatContext* fmtCtx = nullptr;
    AVCodecContext* videoCodecCtx = nullptr;
    AVCodecContext* audioCodecCtx = nullptr;
    int videoStreamIndex = -1;
    int audioStreamIndex = -1;
    VideoFrameConverter m_videoFrameConverter;
    AudioFrameProcessor m_audioFrameProcessor;
    DecodePacer m_decodePacer;
    std::atomic<qint64> seekTargetPtsMs {-1};
    std::atomic<double> playbackSpeed {1.0};
    std::atomic_bool audioSpeedChanged {false};
    std::atomic_bool abortDecode {false};
    std::atomic_bool decoding {false};
    std::atomic_bool audioChainValid {false};
    std::thread decodeThread;
    std::thread previewThread;
    std::mutex previewMutex;
    std::condition_variable previewCv;
    bool previewStopRequested = false;
    bool previewRequestPending = false;
    qint64 previewRequestMs = -1;
    uint64_t previewRequestSerial = 0;
};

#endif
