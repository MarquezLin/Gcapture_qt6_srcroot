#include "capture_runtime_tracker.h"

#include <algorithm>

void CaptureRuntimeTracker::reset()
{
    firstFrameId_ = 0;
    deliveredFrames_ = 0;
    width_ = 0;
    height_ = 0;
    averageFps_ = 0.0;
    stopCallbacks();
    ++sessionId_;
    lastWatchdogFrameCounter_ = 0;
    stallTicks_ = 0;
    stalled_ = false;
}

void CaptureRuntimeTracker::stopCallbacks()
{
    lastVideoPtsNs_ = 0;
    lastPacketPtsNs_ = 0;
    packetLogCount_ = 0;
}

uint64_t &CaptureRuntimeTracker::lastPts(Stream stream)
{
    return stream == Stream::Video ? lastVideoPtsNs_ : lastPacketPtsNs_;
}

void CaptureRuntimeTracker::onFrame(Stream stream, uint64_t ptsNs, uint64_t frameId, int width, int height)
{
    if (frameId != 0)
    {
        if (firstFrameId_ == 0)
            firstFrameId_ = frameId;
        if (frameId >= firstFrameId_)
            deliveredFrames_ = std::max(deliveredFrames_, frameId - firstFrameId_ + 1);
    }
    else
    {
        ++deliveredFrames_;
    }

    width_ = width;
    height_ = height;
    uint64_t &previousPts = lastPts(stream);
    if (previousPts != 0 && ptsNs > previousPts)
    {
        const double measuredFps = 1e9 / double(ptsNs - previousPts);
        if (measuredFps > 0.0)
            averageFps_ = averageFps_ <= 0.0 ? measuredFps : averageFps_ * 0.9 + measuredFps * 0.1;
    }
    previousPts = ptsNs;
}

bool CaptureRuntimeTracker::shouldLogPacket()
{
    ++packetLogCount_;
    return packetLogCount_ <= 5 || packetLogCount_ % 60 == 0;
}

CaptureRuntimeTracker::WatchdogTransition CaptureRuntimeTracker::observeWatchdog(uint64_t frameCounter, bool active)
{
    if (!active)
    {
        stallTicks_ = 0;
        stalled_ = false;
        lastWatchdogFrameCounter_ = frameCounter;
        return WatchdogTransition::None;
    }

    if (frameCounter == lastWatchdogFrameCounter_)
    {
        ++stallTicks_;
        if (stallTicks_ >= 6 && !stalled_)
        {
            stalled_ = true;
            return WatchdogTransition::Stalled;
        }
        return WatchdogTransition::None;
    }

    const bool recovered = stalled_;
    stallTicks_ = 0;
    stalled_ = false;
    lastWatchdogFrameCounter_ = frameCounter;
    return recovered ? WatchdogTransition::Recovered : WatchdogTransition::None;
}
