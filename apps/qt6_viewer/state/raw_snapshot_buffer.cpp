#include "raw_snapshot_buffer.h"

#include <QMutexLocker>
#include <cstring>

void RawSnapshotBuffer::reset()
{
    QMutexLocker lock(&mutex_);
    latest_ = {};
    waiting_ = false;
    frameReady_.wakeAll();
}

bool RawSnapshotBuffer::waitForNextFrame(unsigned long timeoutMs)
{
    QMutexLocker lock(&mutex_);
    latest_ = {};
    waiting_ = true;
    const bool signaled = frameReady_.wait(&mutex_, timeoutMs);
    waiting_ = false;
    return signaled && latest_.isValid();
}

void RawSnapshotBuffer::submit(const gcap_frame_packet_t &packet)
{
    QMutexLocker lock(&mutex_);
    if (!waiting_ || !packet.data[0] || packet.stride[0] <= 0 || packet.height <= 0)
        return;
    if (packet.format != GCAP_FMT_YUY2 && packet.format != GCAP_FMT_Y210 &&
        packet.format != GCAP_FMT_ARGB)
        return;

    latest_.data.resize(packet.stride[0] * packet.height);
    std::memcpy(latest_.data.data(), packet.data[0], static_cast<size_t>(latest_.data.size()));
    latest_.width = packet.width;
    latest_.height = packet.height;
    latest_.stride = packet.stride[0];
    latest_.format = packet.format;
    waiting_ = false;
    frameReady_.wakeAll();
}

RawSnapshotFrame RawSnapshotBuffer::latest() const
{
    QMutexLocker lock(&mutex_);
    return latest_;
}
