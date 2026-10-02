#pragma once

#include <QByteArray>
#include <QMutex>
#include <QWaitCondition>
#include <gcapture.h>

struct RawSnapshotFrame
{
    QByteArray data;
    int width = 0;
    int height = 0;
    int stride = 0;
    gcap_pixfmt_t format = static_cast<gcap_pixfmt_t>(-1);

    bool isValid() const
    {
        return !data.isEmpty() && width > 0 && height > 0 && stride > 0;
    }
};

class RawSnapshotBuffer
{
public:
    void reset();
    bool waitForNextFrame(unsigned long timeoutMs);
    void submit(const gcap_frame_packet_t &packet);
    RawSnapshotFrame latest() const;

private:
    mutable QMutex mutex_;
    QWaitCondition frameReady_;
    bool waiting_ = false;
    RawSnapshotFrame latest_;
};
