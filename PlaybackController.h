#ifndef PLAYBACKCONTROLLER_H
#define PLAYBACKCONTROLLER_H

#include <QAudioSink>
#include <QElapsedTimer>
#include <QImage>
#include <QObject>
#include <QTimer>
#include <deque>
#include <mutex>

#include "AudioOutputDevice.h"
#include "VideoDecoder.h"

class PlaybackController : public QObject
{
    Q_OBJECT

public:
    explicit PlaybackController(QObject* parent = nullptr);
    ~PlaybackController() override;

    bool openMedia(const QString& path);
    bool setPlaying(bool playing);
    void stop();
    void beginSeek();
    void updateSeekPosition(qint64 targetMs);
    void endSeek(qint64 targetMs);
    void setPlaybackRate(double rate);

    bool isMediaLoaded() const;
    bool isPlaying() const;
    qint64 durationMs() const;
    double playbackRate() const;

signals:
    void frameReady(const QImage& image);
    void playbackStateChanged(bool playing);
    void mediaLoadedChanged(bool loaded);
    void positionChanged(qint64 positionMs);
    void durationChanged(qint64 durationMs);
    void timeTextChanged(const QString& text);
    void overlayVisibilityChanged(bool visible);
    void statusMessageChanged(const QString& message);
    void errorOccurred(const QString& message);

private:
    struct RenderFrame {
        QImage image;
        qint64 ptsMs = 0;
    };

    static QString formatTimeText(qint64 currentMs, qint64 durationMs);

    qint64 playbackClockMs() const;
    qint64 audioSinkBufferedMs() const;
    void resetRenderClockCorrection();
    void updateRenderClockCorrection(qint64 masterClockMs, qint64 framePtsMs);
    void emitTimeText(qint64 currentMs);
    void emitPosition(qint64 positionMs);
    void schedulePreview(qint64 targetMs);
    void clearRenderQueue();
    void teardownAudioChain(bool immediate);
    void resetPlaybackState();

private slots:
    void handleMediaInfo(const MediaInfo& info);
    void handleVideoImage(const QImage& image, qint64 ptsMs);
    void handlePreviewImage(const QImage& image, qint64 ptsMs);
    void handleAudioFrame(const std::vector<uint8_t>& pcmData, double ptsSec);
    void onRenderTimerTick();
    void requestPendingPreview();
    void handleDecodeFinished();
    void handleDecoderError(const QString& msg);

private:
    VideoDecoder* m_decoder = nullptr;
    QAudioSink* m_audioSink = nullptr;
    AudioOutputDevice* m_audioOutput = nullptr;
    QTimer* m_renderTimer = nullptr;
    QTimer* m_previewTimer = nullptr;
    std::deque<RenderFrame> m_renderQueue;
    mutable std::mutex m_renderQueueMutex;
    qint64 m_durationMs = 0;
    qint64 m_currentMs = 0;
    qint64 m_clockBaseMs = 0;
    qint64 m_renderClockCorrectionMs = 0;
    qint64 m_pendingPreviewMs = -1;
    bool m_clockRunning = false;
    bool m_userSeeking = false;
    bool m_wasPlayingBeforeSeek = false;
    bool m_mediaLoaded = false;
    QElapsedTimer m_playClock;
    double m_playbackRate = 1.0;
};

#endif // PLAYBACKCONTROLLER_H
