#include "AudioOutputDevice.h"
#include <QMutexLocker>
#include <algorithm>
#include <cstring>

AudioOutputDevice::AudioOutputDevice(const QAudioFormat& format, QObject* parent)
    : QIODevice(parent)
{
    setAudioFormat(format);
}

AudioOutputDevice::~AudioOutputDevice()
{
    beginStop();
    clear();
}

void AudioOutputDevice::beginStop()
{
    m_stopping.store(true, std::memory_order_release);
}

void AudioOutputDevice::appendPcmData(const std::vector<uint8_t>& data, double ptsSec)
{
    if (m_stopping.load(std::memory_order_acquire)) return;
    if (data.empty()) return;

    QMutexLocker lock(&m_mutex);
    m_queue.push_back({data, ptsSec, 0});
    emit readyRead();
}

void AudioOutputDevice::clear()
{
    QMutexLocker lock(&m_mutex);
    m_queue.clear();
    m_currentPtsSec = 0.0;
    m_totalBytesRead = 0;
}

void AudioOutputDevice::setAudioFormat(const QAudioFormat& format)
{
    QMutexLocker lock(&m_mutex);
    const int bytesPerFrame = format.bytesPerFrame();
    const int sampleRate = format.sampleRate();
    if (bytesPerFrame > 0 && sampleRate > 0) {
        m_bytesPerSec = static_cast<double>(bytesPerFrame) * static_cast<double>(sampleRate);
    } else {
        m_bytesPerSec = 0.0;
    }
}

double AudioOutputDevice::getCurrentPtsSec() const
{
    QMutexLocker lock(&m_mutex);
    return m_currentPtsSec;
}

qint64 AudioOutputDevice::bytesToDurationMs(qint64 bytes) const
{
    if (bytes <= 0) {
        return 0;
    }

    QMutexLocker lock(&m_mutex);
    if (m_bytesPerSec <= 0.0) {
        return 0;
    }

    return static_cast<qint64>((static_cast<double>(bytes) * 1000.0) / m_bytesPerSec);
}

qint64 AudioOutputDevice::readData(char* data, qint64 maxlen)
{
    if (!data || maxlen <= 0) return 0;
    if (m_stopping.load(std::memory_order_acquire)) {
        std::memset(data, 0, static_cast<size_t>(maxlen));
        return maxlen;
    }

    qint64 totalWritten = 0;
    QMutexLocker lock(&m_mutex);

    while (totalWritten < maxlen && !m_queue.empty()) {
        AudioChunk& front = m_queue.front();
        size_t available = front.data.size() - front.offset;
        size_t toWrite = std::min(static_cast<size_t>(maxlen - totalWritten), available);

        memcpy(data + totalWritten, front.data.data() + front.offset, toWrite);
        
        front.offset += toWrite;
        totalWritten += toWrite;
        m_totalBytesRead += toWrite;

        if (m_bytesPerSec > 0.0) {
            const double offsetSec = static_cast<double>(front.offset) / m_bytesPerSec;
            m_currentPtsSec = front.ptsSec + offsetSec;
        } else {
            m_currentPtsSec = front.ptsSec;
        }

        if (front.offset >= front.data.size()) {
            m_queue.pop_front();
        }
    }

    if (totalWritten < maxlen) {
        std::memset(data + totalWritten, 0, static_cast<size_t>(maxlen - totalWritten));
        totalWritten = maxlen;
    }

    return totalWritten;
}

qint64 AudioOutputDevice::writeData(const char*, qint64)
{
    return 0; // Read-only
}
