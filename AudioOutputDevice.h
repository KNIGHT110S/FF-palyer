#ifndef AUDIOOUTPUTDEVICE_H
#define AUDIOOUTPUTDEVICE_H

#include <QAudioFormat>
#include <QIODevice>
#include <QMutex>
#include <atomic>
#include <deque>
#include <vector>

class AudioOutputDevice : public QIODevice
{
    Q_OBJECT
public:
    explicit AudioOutputDevice(const QAudioFormat& format);
    ~AudioOutputDevice();

    void beginStop();
    void appendPcmData(const std::vector<uint8_t>& data, double ptsSec);
    void clear();
    void setAudioFormat(const QAudioFormat& format);
    
    // Returns current audio clock in seconds
    double getCurrentPtsSec() const;
    qint64 bytesToDurationMs(qint64 bytes) const;

protected:
    qint64 readData(char* data, qint64 maxlen) override;
    qint64 writeData(const char* data, qint64 len) override;

private:
    struct AudioChunk {
        std::vector<uint8_t> data;
        double ptsSec = 0.0;
        size_t offset = 0;
    };

    std::deque<AudioChunk> m_queue;
    mutable QMutex m_mutex;
    std::atomic_bool m_stopping {false};
    double m_currentPtsSec = 0.0;
    double m_bytesPerSec = 0.0;
    qint64 m_totalBytesRead = 0;
};

#endif // AUDIOOUTPUTDEVICE_H
