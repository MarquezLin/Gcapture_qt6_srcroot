#include "capture_sdk_source.h"

// Uses linked CaptureSDK API (capture_sdk.h)

CaptureSdkSource::CaptureSdkSource(QObject *parent)
    : QObject(parent)
{
}

CaptureSdkSource::~CaptureSdkSource()
{
    stop();
}

bool CaptureSdkSource::isLoaded() const
{
    // Linked against CaptureSDK.lib; if the app starts, the symbols are resolved.
    return true;
}

QString CaptureSdkSource::lastError() const
{
    QMutexLocker lk(&mtx_);
    return lastError_;
}

void CaptureSdkSource::setError(const QString &msg)
{
    {
        QMutexLocker lk(&mtx_);
        lastError_ = msg;
    }
    emit errorOccurred(msg);
}

bool CaptureSdkSource::start(int width, int height)
{
    if (capturing_)
        return true;

    if (!handle_)
    {
        cap_handle_t h = nullptr;
        const cap_result_t rc = cap_create(&h);
        if (rc != CAP_OK || !h)
        {
            setError(QStringLiteral("cap_create failed (%1)").arg((int)rc));
            return false;
        }
        handle_ = h;
    }

    // Try (0,0) as "SDK default" first if caller didn't specify.
    int w = width;
    int h = height;
    if (w < 0) w = 0;
    if (h < 0) h = 0;

    cap_result_t rc = cap_init(handle_, w, h);
    if (rc != CAP_OK)
    {
        // Fallback: a sane default if SDK doesn't accept 0,0.
        if (w == 0 && h == 0)
            rc = cap_init(handle_, 1920, 1080);

        if (rc != CAP_OK)
        {
            setError(QStringLiteral("cap_init failed (%1)").arg((int)rc));
            return false;
        }
    }

    // Use continuous mode for live preview.
    rc = cap_start_capture(handle_, CAP_MODE_CONTINUOUS, &CaptureSdkSource::s_video_cb, this);
    if (rc != CAP_OK)
    {
        setError(QStringLiteral("cap_start_capture failed (%1)").arg((int)rc));
        cap_uninit(handle_);
        return false;
    }

    capturing_ = true;
    return true;
}

void CaptureSdkSource::stop()
{
    if (!handle_)
        return;

    if (capturing_)
    {
        cap_stop_capture(handle_);
        capturing_ = false;
    }

    cap_uninit(handle_);

    cap_destroy(handle_);
    handle_ = nullptr;
}

void CaptureSdkSource::s_video_cb(const uint8_t *buf,
                                 int width,
                                 int height,
                                 int bytes_per_pixel,
                                 void *user)
{
    auto *self = reinterpret_cast<CaptureSdkSource *>(user);
    if (!self)
        return;
    self->onVideo(buf, width, height, bytes_per_pixel);
}

void CaptureSdkSource::onVideo(const uint8_t *buf, int width, int height, int bpp)
{
    if (!buf || width <= 0 || height <= 0)
        return;

    // We intentionally copy the frame: SDK buffer ownership/lifetime is unknown.
    QImage img;
    if (bpp == 4)
    {
        img = QImage(buf, width, height, width * 4, QImage::Format_RGBA8888).copy();
    }
    else if (bpp == 3)
    {
        img = QImage(buf, width, height, width * 3, QImage::Format_RGB888).copy();
    }
    else if (bpp == 2)
    {
        // Common for RGB565; still show something.
        img = QImage(buf, width, height, width * 2, QImage::Format_RGB16).copy();
    }
    else
    {
        // Unknown format; ignore.
        return;
    }

    emit frameReady(img);
}
