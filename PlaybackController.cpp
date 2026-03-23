#include "PlaybackController.h"

#include <QAudioDevice>
#include <QCoreApplication>
#include <QDebug>
#include <QEvent>
#include <QMediaDevices>
#include <QMetaType>

namespace {
constexpr int kRenderTimerIntervalMs = 15;
constexpr int kPreviewDebounceMs = 80;
constexpr qint64 kRenderAheadToleranceMs = 5;
constexpr qint64 kRenderLateDropMs = 120;
constexpr qint64 kMaxRenderClockCorrectionMs = 40;
constexpr qint64 kRenderCorrectionDeadZoneMs = 8;
constexpr size_t kMaxRenderQueueSize = 90;
}

PlaybackController::PlaybackController(QObject* parent)
    : QObject(parent)
    , m_decoder(new VideoDecoder(this))
    , m_renderTimer(new QTimer(this))
    , m_previewTimer(new QTimer(this))
{
    qRegisterMetaType<std::vector<uint8_t>>("std::vector<uint8_t>");

    connect(m_decoder, &VideoDecoder::mediaInfoReady,
            this, &PlaybackController::handleMediaInfo,
            Qt::QueuedConnection);
    connect(m_decoder, &VideoDecoder::videoImageReady,
            this, &PlaybackController::handleVideoImage,
            Qt::QueuedConnection);
    connect(m_decoder, &VideoDecoder::previewImageReady,
            this, &PlaybackController::handlePreviewImage,
            Qt::QueuedConnection);
    connect(m_decoder, &VideoDecoder::audioFrameDecoded,
            this, &PlaybackController::handleAudioFrame,
            Qt::QueuedConnection);
    connect(m_decoder, &VideoDecoder::decodeFinished,
            this, &PlaybackController::handleDecodeFinished,
            Qt::QueuedConnection);
    connect(m_decoder, &VideoDecoder::errorOccurred,
            this, &PlaybackController::handleDecoderError,
            Qt::QueuedConnection);

    m_renderTimer->setInterval(kRenderTimerIntervalMs);
    connect(m_renderTimer, &QTimer::timeout,
            this, &PlaybackController::onRenderTimerTick);
    m_renderTimer->start();

    m_previewTimer->setSingleShot(true);
    m_previewTimer->setInterval(kPreviewDebounceMs);
    connect(m_previewTimer, &QTimer::timeout,
            this, &PlaybackController::requestPendingPreview);
}

PlaybackController::~PlaybackController()
{
    teardownAudioChain(true);
}

bool PlaybackController::openMedia(const QString& path)
{
    if (path.isEmpty()) {
        return false;
    }

    if (m_previewTimer) {
        m_previewTimer->stop();
    }
    m_pendingPreviewMs = -1;
    m_decoder->cancelPendingPreview();
    m_decoder->stopDecoding();
    QCoreApplication::removePostedEvents(this, QEvent::MetaCall);

    teardownAudioChain(true);
    clearRenderQueue();
    resetPlaybackState();
    emit frameReady(QImage());
    emit overlayVisibilityChanged(false);

    if (!m_decoder->open(path)) {
        emit statusMessageChanged(QStringLiteral("Open video failed"));
        emit overlayVisibilityChanged(true);
        return false;
    }

    m_mediaLoaded = true;
    emit mediaLoadedChanged(true);

    if (!m_decoder->loadPreviewFrame(0)) {
        emit statusMessageChanged(
            QStringLiteral("Opened video: %1 (failed to load first-frame preview)").arg(path));
        emit overlayVisibilityChanged(true);
        return true;
    }

    emit statusMessageChanged(QStringLiteral("Opened video: %1").arg(path));
    emit overlayVisibilityChanged(true);
    return true;
}

bool PlaybackController::setPlaying(bool playing)
{
    if (playing) {
        emit overlayVisibilityChanged(false);
        if (!m_decoder->startDecoding()) {
            emit playbackStateChanged(false);
            if (!m_userSeeking) {
                emit overlayVisibilityChanged(true);
            }
            return false;
        }

        m_clockBaseMs = m_currentMs;
        m_playClock.restart();
        m_clockRunning = true;
        resetRenderClockCorrection();
        if (m_audioSink) {
            m_audioSink->resume();
        }

        emit playbackStateChanged(true);
        emit statusMessageChanged(QStringLiteral("Decoding started."));
        return true;
    }

    m_decoder->stopDecoding();
    m_clockBaseMs = m_currentMs;
    if (m_audioSink) {
        m_audioSink->suspend();
    }

    m_clockRunning = false;
    emit playbackStateChanged(false);
    if (!m_userSeeking) {
        emit overlayVisibilityChanged(true);
    }
    emit statusMessageChanged(QStringLiteral("Paused."));
    return true;
}

void PlaybackController::stop()
{
    if (m_previewTimer) {
        m_previewTimer->stop();
    }
    m_pendingPreviewMs = -1;
    m_decoder->cancelPendingPreview();
    m_decoder->stopDecoding();
    QCoreApplication::removePostedEvents(this, QEvent::MetaCall);
    clearRenderQueue();
    if (m_audioSink) {
        m_audioSink->stop();
        m_audioSink->reset();
    }

    m_userSeeking = false;
    m_wasPlayingBeforeSeek = false;
    m_currentMs = 0;
    m_clockBaseMs = 0;
    m_clockRunning = false;
    resetRenderClockCorrection();

    emit playbackStateChanged(false);
    emit frameReady(QImage());
    emitPosition(0);
    emitTimeText(0);
    emit overlayVisibilityChanged(false);
    emit statusMessageChanged(QStringLiteral("Stopped."));
}

void PlaybackController::beginSeek()
{
    if (!m_mediaLoaded) {
        return;
    }

    m_userSeeking = true;
    m_pendingPreviewMs = -1;
    if (m_previewTimer) {
        m_previewTimer->stop();
    }
    m_decoder->cancelPendingPreview();
    emit overlayVisibilityChanged(false);

    m_wasPlayingBeforeSeek = m_clockRunning;
    if (m_wasPlayingBeforeSeek) {
        setPlaying(false);
    }
}

void PlaybackController::updateSeekPosition(qint64 targetMs)
{
    if (!m_userSeeking) {
        return;
    }

    const qint64 boundedMs = targetMs < 0 ? 0 : targetMs;
    m_currentMs = boundedMs;
    m_clockBaseMs = boundedMs;
    emitTimeText(boundedMs);
    schedulePreview(boundedMs);
}

void PlaybackController::endSeek(qint64 targetMs)
{
    if (!m_userSeeking) {
        return;
    }

    m_userSeeking = false;
    if (m_previewTimer) {
        m_previewTimer->stop();
    }
    m_decoder->cancelPendingPreview();

    const qint64 boundedMs = targetMs < 0 ? 0 : targetMs;
    m_clockRunning = false;
    QCoreApplication::removePostedEvents(this, QEvent::MetaCall);
    clearRenderQueue();

    m_currentMs = boundedMs;
    m_clockBaseMs = boundedMs;
    resetRenderClockCorrection();
    emitPosition(m_currentMs);
    emitTimeText(m_currentMs);

    if (m_wasPlayingBeforeSeek) {
        if (!m_decoder->seekMs(boundedMs)) {
            emit overlayVisibilityChanged(true);
            emit statusMessageChanged(QStringLiteral("Seek failed."));
            return;
        }

        if (!setPlaying(true)) {
            return;
        }

        emit statusMessageChanged(QStringLiteral("Jumped to %1 ms").arg(boundedMs));
        return;
    }

    m_decoder->requestPreviewFrame(boundedMs);
    emit overlayVisibilityChanged(true);
    emit statusMessageChanged(QStringLiteral("Preview moved to %1 ms").arg(boundedMs));
}

void PlaybackController::setPlaybackRate(double rate)
{
    if (qAbs(m_playbackRate - rate) < 0.001) {
        return;
    }

    if (m_clockRunning) {
        m_clockBaseMs = playbackClockMs();
        m_playClock.restart();
    }

    m_playbackRate = rate;
    m_decoder->setPlaybackSpeed(rate);
    if (m_audioOutput) {
        m_audioOutput->clear();
    }
    resetRenderClockCorrection();
    if (m_audioSink && m_audioOutput) {
        m_audioSink->reset();
        m_audioSink->start(m_audioOutput);
        if (!m_clockRunning) {
            m_audioSink->suspend();
        }
    }

    emit statusMessageChanged(QStringLiteral("Playback speed: %1x").arg(rate));
}

bool PlaybackController::isMediaLoaded() const
{
    return m_mediaLoaded;
}

bool PlaybackController::isPlaying() const
{
    return m_clockRunning;
}

qint64 PlaybackController::durationMs() const
{
    return m_durationMs;
}

double PlaybackController::playbackRate() const
{
    return m_playbackRate;
}

QString PlaybackController::formatTimeText(qint64 currentMs, qint64 durationMs)
{
    qint64 boundedCurrentMs = currentMs < 0 ? 0 : currentMs;
    if (durationMs > 0 && boundedCurrentMs > durationMs) {
        boundedCurrentMs = durationMs;
    }

    const qint64 currentSec = boundedCurrentMs / 1000;
    const qint64 currentMinutes = currentSec / 60;
    const qint64 currentSeconds = currentSec % 60;

    const qint64 totalSec = durationMs / 1000;
    const qint64 totalMinutes = totalSec / 60;
    const qint64 totalSeconds = totalSec % 60;

    const QString currentText = QStringLiteral("%1:%2")
                                    .arg(currentMinutes, 2, 10, QLatin1Char('0'))
                                    .arg(currentSeconds, 2, 10, QLatin1Char('0'));
    const QString totalText = QStringLiteral("%1:%2")
                                  .arg(totalMinutes, 2, 10, QLatin1Char('0'))
                                  .arg(totalSeconds, 2, 10, QLatin1Char('0'));
    return QStringLiteral("%1 / %2").arg(currentText, totalText);
}

qint64 PlaybackController::playbackClockMs() const
{
    if (m_audioOutput && m_audioSink && m_clockRunning) {
        const QAudio::State state = m_audioSink->state();
        if (state == QAudio::ActiveState || state == QAudio::IdleState) {
            const double audioPtsSec = m_audioOutput->getCurrentPtsSec();
            if (audioPtsSec > 0.0) {
                qint64 playedMs = static_cast<qint64>(audioPtsSec * 1000.0) - audioSinkBufferedMs();
                if (playedMs < 0) {
                    playedMs = 0;
                }
                if (m_durationMs > 0 && playedMs > m_durationMs) {
                    playedMs = m_durationMs;
                }
                return playedMs;
            }
        }
    }

    if (!m_clockRunning) {
        return m_clockBaseMs;
    }
    return m_clockBaseMs + static_cast<qint64>(m_playClock.elapsed() * m_playbackRate);
}

qint64 PlaybackController::audioSinkBufferedMs() const
{
    if (!m_audioSink || !m_audioOutput) {
        return 0;
    }

    const qint64 bufferSize = m_audioSink->bufferSize();
    const qint64 bytesFree = m_audioSink->bytesFree();

    if (bufferSize <= 0 || bytesFree < 0) {
        return 0;
    }

    const qint64 bufferedBytes = qBound<qint64>(0, bufferSize - bytesFree, bufferSize);
    return m_audioOutput->bytesToDurationMs(bufferedBytes);
}

void PlaybackController::resetRenderClockCorrection()
{
    m_renderClockCorrectionMs = 0;
}

void PlaybackController::updateRenderClockCorrection(qint64 masterClockMs, qint64 framePtsMs)
{
    const qint64 presentationErrorMs = masterClockMs - framePtsMs;
    if (qAbs(presentationErrorMs) <= kRenderCorrectionDeadZoneMs) {
        return;
    }

    const qint64 blendedCorrection =
        (m_renderClockCorrectionMs * 3 + presentationErrorMs) / 4;
    m_renderClockCorrectionMs = qBound<qint64>(
        -kMaxRenderClockCorrectionMs,
        blendedCorrection,
        kMaxRenderClockCorrectionMs);
}

void PlaybackController::emitTimeText(qint64 currentMs)
{
    emit timeTextChanged(formatTimeText(currentMs, m_durationMs));
}

void PlaybackController::emitPosition(qint64 positionMs)
{
    qint64 boundedMs = positionMs < 0 ? 0 : positionMs;
    if (m_durationMs > 0 && boundedMs > m_durationMs) {
        boundedMs = m_durationMs;
    }
    emit positionChanged(boundedMs);
}

void PlaybackController::schedulePreview(qint64 targetMs)
{
    if (!m_userSeeking || !m_mediaLoaded || !m_previewTimer) {
        return;
    }

    m_pendingPreviewMs = targetMs < 0 ? 0 : targetMs;
    m_previewTimer->start();
}

void PlaybackController::clearRenderQueue()
{
    std::lock_guard<std::mutex> lock(m_renderQueueMutex);
    m_renderQueue.clear();
    resetRenderClockCorrection();
    if (m_audioOutput) {
        m_audioOutput->clear();
    }
}

void PlaybackController::teardownAudioChain(bool immediate)
{
    Q_UNUSED(immediate);

    QAudioSink* sink = m_audioSink;
    AudioOutputDevice* output = m_audioOutput;
    const bool outputOwnedBySink = sink && output && output->parent() == sink;

    m_audioSink = nullptr;
    m_audioOutput = nullptr;

    if (output) {
        output->beginStop();
        output->clear();
    }
    if (sink) {
        sink->stop();
        sink->reset();
    }
    if (output && output->isOpen()) {
        output->close();
    }
    if (sink) {
        delete sink;
    }
    if (!outputOwnedBySink && output) {
        delete output;
    }
}

void PlaybackController::resetPlaybackState()
{
    m_durationMs = 0;
    m_currentMs = 0;
    m_clockBaseMs = 0;
    m_clockRunning = false;
    m_userSeeking = false;
    m_wasPlayingBeforeSeek = false;
    m_mediaLoaded = false;
    resetRenderClockCorrection();
    emit playbackStateChanged(false);
    emit mediaLoadedChanged(false);
    emit durationChanged(0);
    emitPosition(0);
    emitTimeText(0);
}

void PlaybackController::handleMediaInfo(const MediaInfo& info)
{
    m_durationMs = info.durationMs;
    m_currentMs = 0;
    m_clockBaseMs = 0;
    m_clockRunning = false;
    resetRenderClockCorrection();

    teardownAudioChain(true);

    if (info.hasAudio) {
        QAudioFormat format;
        format.setSampleRate(44100);
        format.setChannelCount(2);
        format.setSampleFormat(QAudioFormat::Int16);

        QAudioDevice device = QMediaDevices::defaultAudioOutput();
        if (!device.isFormatSupported(format)) {
            qWarning() << "Default audio format not supported, trying nearest";
            format = device.preferredFormat();
        }

        if (!m_decoder->setAudioOutputFormat(format)) {
            emit durationChanged(info.durationMs);
            emitPosition(0);
            emitTimeText(0);
            return;
        }

        m_audioSink = new QAudioSink(device, format, this);
        m_audioOutput = new AudioOutputDevice(format, m_audioSink);

        const qint32 bufferSize = format.bytesForDuration(1000000);
        if (bufferSize > 0) {
            m_audioSink->setBufferSize(bufferSize);
        }

        if (!m_audioOutput->open(QIODevice::ReadOnly)) {
            qWarning() << "Failed to open AudioOutputDevice";
        }
        m_audioSink->start(m_audioOutput);
        m_audioSink->suspend();
    }

    emit durationChanged(info.durationMs);
    emitPosition(0);
    emitTimeText(0);
}

void PlaybackController::handleVideoImage(const QImage& image, qint64 ptsMs)
{
    if (!m_clockRunning) {
        m_currentMs = ptsMs < 0 ? 0 : ptsMs;
        if (m_durationMs > 0 && m_currentMs > m_durationMs) {
            m_currentMs = m_durationMs;
        }

        emit frameReady(image);
        emitPosition(m_currentMs);
        emitTimeText(m_currentMs);
        return;
    }

    std::lock_guard<std::mutex> lock(m_renderQueueMutex);
    if (m_renderQueue.size() >= kMaxRenderQueueSize) {
        m_renderQueue.pop_front();
    }
    m_renderQueue.push_back(RenderFrame{image, ptsMs});
}

void PlaybackController::handlePreviewImage(const QImage& image, qint64 ptsMs)
{
    if (m_clockRunning) {
        return;
    }

    emit frameReady(image);

    if (!m_userSeeking) {
        m_currentMs = ptsMs < 0 ? 0 : ptsMs;
        m_clockBaseMs = m_currentMs;
        emitPosition(m_currentMs);
        emitTimeText(m_currentMs);
    }
}

void PlaybackController::handleAudioFrame(const std::vector<uint8_t>& pcmData, double ptsSec)
{
    AudioOutputDevice* audioOutput = m_audioOutput;
    QAudioSink* audioSink = m_audioSink;
    if (!audioOutput || !audioSink) {
        return;
    }

    audioOutput->appendPcmData(pcmData, ptsSec);
    if (m_clockRunning && audioSink->state() == QAudio::SuspendedState) {
        audioSink->resume();
    }
}

void PlaybackController::onRenderTimerTick()
{
    if (!m_clockRunning) {
        return;
    }

    const qint64 masterClockMs = playbackClockMs();
    const qint64 renderClockMs = masterClockMs + m_renderClockCorrectionMs;
    RenderFrame frameToRender;
    bool hasFrame = false;

    {
        std::lock_guard<std::mutex> lock(m_renderQueueMutex);
        while (!m_renderQueue.empty()) {
            if (m_renderQueue.size() > kMaxRenderQueueSize) {
                m_renderQueue.pop_front();
                continue;
            }

            const RenderFrame& front = m_renderQueue.front();
            if (front.ptsMs + kRenderLateDropMs < masterClockMs && m_renderQueue.size() > 1) {
                m_renderQueue.pop_front();
                continue;
            }
            if (front.ptsMs <= renderClockMs + kRenderAheadToleranceMs) {
                frameToRender = front;
                m_renderQueue.pop_front();
                hasFrame = true;
            }
            break;
        }
    }

    if (!hasFrame) {
        return;
    }

    emit frameReady(frameToRender.image);
    updateRenderClockCorrection(masterClockMs, frameToRender.ptsMs);
    m_currentMs = frameToRender.ptsMs;
    emitPosition(m_currentMs);
    emitTimeText(m_currentMs);
}

void PlaybackController::requestPendingPreview()
{
    if (!m_userSeeking || !m_mediaLoaded || m_pendingPreviewMs < 0) {
        return;
    }

    m_decoder->requestPreviewFrame(m_pendingPreviewMs);
}

void PlaybackController::handleDecodeFinished()
{
    m_clockBaseMs = m_currentMs;
    m_clockRunning = false;
    resetRenderClockCorrection();
    if (m_audioSink) {
        m_audioSink->suspend();
    }

    emit playbackStateChanged(false);
    emit overlayVisibilityChanged(true);
    emit statusMessageChanged(QStringLiteral("Decoding finished."));
}

void PlaybackController::handleDecoderError(const QString& msg)
{
    emit errorOccurred(msg);
    emit statusMessageChanged(msg);
}
