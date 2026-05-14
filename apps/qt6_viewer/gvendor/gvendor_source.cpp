#include "gvendor_source.h"

#include <QMutexLocker>
#include <algorithm>
#include <chrono>

namespace
{
    static int clamp_u8(int v)
    {
        return (std::max)(0, (std::min)(255, v));
    }

    static void yuv_to_rgb(int y, int u, int v, uchar &r, uchar &g, uchar &b)
    {
        const int c = y - 16;
        const int d = u - 128;
        const int e = v - 128;
        r = static_cast<uchar>(clamp_u8((298 * c + 409 * e + 128) >> 8));
        g = static_cast<uchar>(clamp_u8((298 * c - 100 * d - 208 * e + 128) >> 8));
        b = static_cast<uchar>(clamp_u8((298 * c + 516 * d + 128) >> 8));
    }

    static QString gv_error(gv_status_t st, gv_handle h)
    {
        const char *detail = h ? gv_last_error(h) : "";
        if (detail && detail[0])
            return QStringLiteral("%1 (%2)").arg(QString::fromUtf8(gv_strerror(st)), QString::fromUtf8(detail));
        return QString::fromUtf8(gv_strerror(st));
    }
}

GVendorSource::GVendorSource(QObject *parent)
    : QObject(parent)
{
}

GVendorSource::~GVendorSource()
{
    stop();
}

bool GVendorSource::isLoaded() const
{
    return true;
}

QString GVendorSource::lastError() const
{
    QMutexLocker lk(&mtx_);
    return lastError_;
}

QString GVendorSource::deviceName() const
{
    QMutexLocker lk(&mtx_);
    return deviceName_;
}

void GVendorSource::setError(const QString &msg)
{
    {
        QMutexLocker lk(&mtx_);
        lastError_ = msg;
    }
    emit errorOccurred(msg);
}

bool GVendorSource::start(int width, int height)
{
    if (capturing_)
        return true;

    gv_status_t st = gv_open_default(&handle_);
    if (st != GV_OK || !handle_)
    {
        setError(QStringLiteral("gv_open_default failed: %1").arg(gv_error(st, handle_)));
        return false;
    }

    gv_device_info_t info = {};
    if (gv_get_device_info(handle_, &info) == GV_OK)
    {
        QMutexLocker lk(&mtx_);
        deviceName_ = QString::fromUtf8(info.friendly_name);
    }

    st = gv_set_input(handle_, GDRIVER_INPUT_SDI, 0);
    if (st != GV_OK)
    {
        setError(QStringLiteral("gv_set_input(SDI) failed: %1").arg(gv_error(st, handle_)));
        stop();
        return false;
    }

    gv_stream_desc_t desc = {};
    desc.channel_index = 0;
    desc.input = GDRIVER_INPUT_SDI;
    desc.width = width > 0 ? static_cast<uint32_t>(width) : 1920u;
    desc.height = height > 0 ? static_cast<uint32_t>(height) : 1080u;
    desc.fps_num = 30000;
    desc.fps_den = 1001;
    desc.pixel_format = GDRIVER_PIXFMT_YUY2;
    desc.buffer_count = 1;
    desc.memory_kind = GDRIVER_MEMORY_DRIVER_COPY;

    st = gv_configure_stream(handle_, &desc);
    if (st != GV_OK)
    {
        setError(QStringLiteral("gv_configure_stream failed: %1").arg(gv_error(st, handle_)));
        stop();
        return false;
    }

    st = gv_start_stream(handle_);
    if (st != GV_OK)
    {
        setError(QStringLiteral("gv_start_stream failed: %1").arg(gv_error(st, handle_)));
        stop();
        return false;
    }

    capturing_ = true;
    running_ = true;
    pumpThread_ = std::thread([this]() { captureLoop(); });
    return true;
}

void GVendorSource::stop()
{
    running_ = false;

    if (pumpThread_.joinable())
        pumpThread_.join();

    if (handle_)
    {
        gv_stop_stream(handle_);
        gv_close(handle_);
        handle_ = nullptr;
    }

    capturing_ = false;
}

void GVendorSource::captureLoop()
{
    while (running_)
    {
        gv_frame_t frame = {};
        const gv_status_t st = gv_wait_frame(handle_, 1000, &frame);
        if (!running_)
            break;
        if (st == GV_OK)
        {
            onFrame(frame);
            gv_release_frame(handle_, &frame);
            continue;
        }
        if (st == GV_ETIMEOUT)
            continue;

        setError(QStringLiteral("gv_wait_frame failed: %1").arg(gv_error(st, handle_)));
        break;
    }
}

void GVendorSource::onFrame(const gv_frame_t &frame)
{
    QImage img = convertFrameToImage(frame);
    if (!img.isNull())
        emit frameReady(img);
}

QImage GVendorSource::convertFrameToImage(const gv_frame_t &frame) const
{
    if (!frame.data || frame.width == 0 || frame.height == 0)
        return QImage();

    const int width = static_cast<int>(frame.width);
    const int height = static_cast<int>(frame.height);
    const uchar *src = static_cast<const uchar *>(frame.data);

    if (frame.pixel_format == GDRIVER_PIXFMT_RGB24)
        return QImage(src, width, height, static_cast<int>(frame.plane_stride_bytes[0]), QImage::Format_RGB888).copy();

    if (frame.pixel_format != GDRIVER_PIXFMT_YUY2)
        return QImage();

    QImage img(width, height, QImage::Format_RGB888);
    const int srcStride = frame.plane_stride_bytes[0] ? static_cast<int>(frame.plane_stride_bytes[0]) : width * 2;

    for (int y = 0; y < height; ++y)
    {
        const uchar *row = src + y * srcStride;
        uchar *dst = img.scanLine(y);
        for (int x = 0; x + 1 < width; x += 2)
        {
            const int y0 = row[x * 2 + 0];
            const int u = row[x * 2 + 1];
            const int y1 = row[x * 2 + 2];
            const int v = row[x * 2 + 3];

            yuv_to_rgb(y0, u, v, dst[x * 3 + 0], dst[x * 3 + 1], dst[x * 3 + 2]);
            yuv_to_rgb(y1, u, v, dst[(x + 1) * 3 + 0], dst[(x + 1) * 3 + 1], dst[(x + 1) * 3 + 2]);
        }
    }

    return img;
}
