#pragma once

#include <cstdint>

class CaptureRuntimeTracker
{
public:
    enum class Stream
    {
        Video,
        Packet
    };

    enum class WatchdogTransition
    {
        None,
        Stalled,
        Recovered
    };

    void reset();
    void stopCallbacks();
    void onFrame(Stream stream, uint64_t ptsNs, uint64_t frameId, int width, int height);
    bool shouldLogPacket();
    WatchdogTransition observeWatchdog(uint64_t frameCounter, bool active);

    uint64_t deliveredFrames() const { return deliveredFrames_; }
    int width() const { return width_; }
    int height() const { return height_; }
    double fps() const { return averageFps_; }
    uint64_t sessionId() const { return sessionId_; }
    uint64_t packetLogCount() const { return packetLogCount_; }
    int stallTicks() const { return stallTicks_; }
    bool stalled() const { return stalled_; }

private:
    uint64_t &lastPts(Stream stream);

    uint64_t firstFrameId_ = 0;
    uint64_t deliveredFrames_ = 0;
    int width_ = 0;
    int height_ = 0;
    double averageFps_ = 0.0;
    uint64_t lastVideoPtsNs_ = 0;
    uint64_t lastPacketPtsNs_ = 0;
    uint64_t packetLogCount_ = 0;
    uint64_t sessionId_ = 0;
    uint64_t lastWatchdogFrameCounter_ = 0;
    int stallTicks_ = 0;
    bool stalled_ = false;
};
