#include "DecodePacer.h"

#include <thread>

void DecodePacer::reset()
{
    m_clockReady = false;
    m_clockStartPtsMs = 0;
    m_seekFloorPtsMs = -1;
    m_clockStartTime = std::chrono::steady_clock::time_point{};
    m_speedChanged.store(false);
}

void DecodePacer::setSeekFloor(qint64 targetMs)
{
    m_seekFloorPtsMs = targetMs < 0 ? 0 : targetMs;
}

void DecodePacer::setPlaybackSpeed(double speed)
{
    if (speed <= 0.0) {
        return;
    }

    m_playbackSpeed.store(speed);
    m_speedChanged.store(true);
}

void DecodePacer::pace(qint64 ptsMs, const std::atomic_bool& abortFlag)
{
    if (ptsMs < 0) {
        return;
    }

    constexpr qint64 kPacingStartToleranceMs = 80;
    if (m_seekFloorPtsMs >= 0 && ptsMs + kPacingStartToleranceMs < m_seekFloorPtsMs) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    if (!m_clockReady || m_speedChanged.exchange(false)) {
        m_clockReady = true;
        m_clockStartPtsMs = ptsMs;
        m_clockStartTime = now;
        m_seekFloorPtsMs = -1;
        return;
    }

    const qint64 ptsElapsedMs = ptsMs - m_clockStartPtsMs;
    if (ptsElapsedMs <= 0) {
        return;
    }

    const double currentSpeed = m_playbackSpeed.load();
    const auto realTimeElapsedMs = static_cast<long long>(ptsElapsedMs / currentSpeed);
    const auto targetTime = m_clockStartTime + std::chrono::milliseconds(realTimeElapsedMs);
    if (targetTime <= now) {
        return;
    }

    auto waitDuration = targetTime - now;
    constexpr auto kMaxSingleWait = std::chrono::milliseconds(80);
    if (waitDuration > kMaxSingleWait) {
        m_clockStartPtsMs = ptsMs;
        m_clockStartTime = now;
        return;
    }

    constexpr auto kSleepSlice = std::chrono::milliseconds(5);
    while (waitDuration > std::chrono::milliseconds::zero() && !abortFlag.load()) {
        const auto slice = waitDuration > kSleepSlice ? kSleepSlice : waitDuration;
        std::this_thread::sleep_for(slice);
        waitDuration -= slice;
    }
}
