#ifndef DECODEPACER_H
#define DECODEPACER_H

#include <QtGlobal>
#include <atomic>
#include <chrono>

class DecodePacer
{
public:
    void reset();
    void setSeekFloor(qint64 targetMs);
    void setPlaybackSpeed(double speed);
    void pace(qint64 ptsMs, const std::atomic_bool& abortFlag);

private:
    bool m_clockReady = false;
    qint64 m_clockStartPtsMs = 0;
    qint64 m_seekFloorPtsMs = -1;
    std::atomic<double> m_playbackSpeed {1.0};
    std::atomic_bool m_speedChanged {false};
    std::chrono::steady_clock::time_point m_clockStartTime;
};

#endif // DECODEPACER_H
