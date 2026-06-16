#include "gvfg_source.h"

#include <algorithm>

#ifdef QT6_VIEWER_USE_STANDALONE_GVFG
namespace
{
QString gvfgEventName(gvfg_event_type_t type)
{
    switch (type)
    {
    case GVFG_EVENT_PLUG_IN:
        return QStringLiteral("PLUG_IN");
    case GVFG_EVENT_PLUG_OUT:
        return QStringLiteral("PLUG_OUT");
    case GVFG_EVENT_CAPTURE_PAUSED:
        return QStringLiteral("CAPTURE_PAUSED");
    case GVFG_EVENT_CAPTURE_RESUMED:
        return QStringLiteral("CAPTURE_RESUMED");
    default:
        return QStringLiteral("UNKNOWN");
    }
}

QImage frameToImage(const gvfg_frame_t &frame)
{
    if (!frame.data || frame.width <= 0 || frame.height <= 0)
        return QImage();

    switch (frame.pixel_format)
    {
    case GVFG_PIXFMT_BGRA8:
    {
        const int stride = frame.width * 4;
        if (frame.data_size < static_cast<uint64_t>(stride) * static_cast<uint64_t>(frame.height))
            return QImage();
        const QImage image(static_cast<const uchar *>(frame.data), frame.width, frame.height, stride, QImage::Format_ARGB32);
        return image.copy();
    }
    case GVFG_PIXFMT_BGRX32:
    {
        const int stride = frame.width * 4;
        if (frame.data_size < static_cast<uint64_t>(stride) * static_cast<uint64_t>(frame.height))
            return QImage();
        const QImage image(static_cast<const uchar *>(frame.data), frame.width, frame.height, stride, QImage::Format_RGB32);
        return image.copy();
    }
    case GVFG_PIXFMT_RGB24:
    {
        const int stride = frame.width * 3;
        if (frame.data_size < static_cast<uint64_t>(stride) * static_cast<uint64_t>(frame.height))
            return QImage();
        const QImage image(static_cast<const uchar *>(frame.data), frame.width, frame.height, stride, QImage::Format_RGB888);
        return image.copy();
    }
    default:
        return QImage();
    }
}
}
#endif

GvfgSource::GvfgSource(QObject *parent)
    : QObject(parent)
{
}

GvfgSource::~GvfgSource()
{
    stop();
}

QStringList GvfgSource::enumerateDevices()
{
    gvfg_device_info_t devices[GVFG_MAX_DEVICES] = {};
    const int n = gvfg_enumerate_devices(devices, GVFG_MAX_DEVICES);
    QStringList names;
    for (int i = 0; i < n; ++i)
    {
        const QString name = QString::fromUtf8(devices[i].name);
        names << (name.isEmpty() ? QStringLiteral("GVFG Capture") : name);
    }
    return names;
}

bool GvfgSource::start(void *previewHwnd, int deviceIndex, int previewBitDepthMode)
{
    stop();

    gvfg_status_t st = gvfg_create(&handle_);
    if (st != GVFG_OK || !handle_)
    {
        emit errorOccurred(QStringLiteral("gvfg_create failed: %1").arg(QString::fromUtf8(gvfg_strerror(st))));
        handle_ = nullptr;
        return false;
    }

#ifdef QT6_VIEWER_USE_STANDALONE_GVFG
    if (!setPreview(previewHwnd, previewBitDepthMode))
    {
        stop();
        return false;
    }
#else
    gvfg_set_callbacks(handle_, &GvfgSource::onFrame, &GvfgSource::onError, this);

    if (!setPreview(previewHwnd, previewBitDepthMode))
    {
        stop();
        return false;
    }
#endif

    st = gvfg_open(handle_, std::max(0, deviceIndex));
    if (st != GVFG_OK)
    {
        emit errorOccurred(QStringLiteral("gvfg_open failed: %1").arg(QString::fromUtf8(gvfg_strerror(st))));
        stop();
        return false;
    }

    gvfg_runtime_info_t preStartInfo{};
    if (gvfg_get_runtime_info(handle_, &preStartInfo) == GVFG_OK)
        emit preStartRuntimeInfoReady(preStartInfo);

    st = gvfg_start(handle_);
    if (st != GVFG_OK)
    {
        emit errorOccurred(QStringLiteral("gvfg_start failed: %1").arg(QString::fromUtf8(gvfg_strerror(st))));
        stop();
        return false;
    }

    running_ = true;
#ifdef QT6_VIEWER_USE_STANDALONE_GVFG
    stopRequested_.store(false, std::memory_order_release);
    readThread_ = std::thread([this]() { readLoop(); });
#endif
    return true;
}

bool GvfgSource::setPreview(void *previewHwnd, int previewBitDepthMode)
{
    if (!handle_)
        return false;

#ifdef QT6_VIEWER_USE_STANDALONE_GVFG
    Q_UNUSED(previewBitDepthMode);

    if (!previewHwnd)
    {
        if (previewHandle_)
        {
            gvfg_preview_destroy(previewHandle_);
            previewHandle_ = nullptr;
        }
        return true;
    }

    if (!previewHandle_)
    {
        const gvfg_preview_status_t st = gvfg_preview_create(&previewHandle_);
        if (st != GVFG_PREVIEW_OK || !previewHandle_)
        {
            emit errorOccurred(QStringLiteral("gvfg_preview_create failed: %1").arg(QString::fromUtf8(gvfg_preview_strerror(st))));
            previewHandle_ = nullptr;
            return false;
        }
    }

    const gvfg_preview_status_t st = gvfg_preview_attach_window(previewHandle_, previewHwnd);
    if (st != GVFG_PREVIEW_OK)
    {
        emit errorOccurred(QStringLiteral("gvfg_preview_attach_window failed: %1").arg(QString::fromUtf8(gvfg_preview_strerror(st))));
        return false;
    }
    return true;
#else
    gvfg_preview_desc_t preview{};
    preview.hwnd = previewHwnd;
    preview.enable_preview = previewHwnd ? 1 : 0;
    preview.swapchain_bitdepth = previewBitDepthMode;
    const gvfg_status_t st = gvfg_set_preview(handle_, &preview);
    if (st != GVFG_OK)
    {
        emit errorOccurred(QStringLiteral("gvfg_set_preview failed: %1").arg(QString::fromUtf8(gvfg_strerror(st))));
        return false;
    }
    return true;
#endif
}

void GvfgSource::stop()
{
    running_ = false;
#ifdef QT6_VIEWER_USE_STANDALONE_GVFG
    stopRequested_.store(true, std::memory_order_release);
    if (readThread_.joinable())
        readThread_.join();
    if (previewHandle_)
    {
        gvfg_preview_destroy(previewHandle_);
        previewHandle_ = nullptr;
    }
#endif
    if (handle_)
    {
        gvfg_stop(handle_);
        gvfg_destroy(handle_);
        handle_ = nullptr;
    }
}

gvfg_signal_status_t GvfgSource::signalStatus() const
{
    gvfg_signal_status_t status{};
    if (handle_)
        gvfg_get_signal_status(handle_, &status);
    return status;
}

gvfg_runtime_info_t GvfgSource::runtimeInfo() const
{
    gvfg_runtime_info_t info{};
    if (handle_)
        gvfg_get_runtime_info(handle_, &info);
    return info;
}

gvfg_preview_info_t GvfgSource::previewInfo() const
{
    gvfg_preview_info_t info{};
#ifdef QT6_VIEWER_USE_STANDALONE_GVFG
    if (previewHandle_)
        gvfg_preview_get_info(previewHandle_, &info);
#else
    if (handle_)
        gvfg_get_preview_info(handle_, &info);
#endif
    return info;
}

#ifdef QT6_VIEWER_USE_STANDALONE_GVFG
void GvfgSource::readLoop()
{
    while (!stopRequested_.load(std::memory_order_acquire))
    {
        gvfg_event_t event{};
        while (handle_ && gvfg_poll_event(handle_, &event, 0) == GVFG_OK)
        {
            emit errorOccurred(QStringLiteral("GVFG event %1 ts=%2")
                                   .arg(gvfgEventName(event.type))
                                   .arg(static_cast<qulonglong>(event.timestamp_ns)));
        }

        gvfg_frame_t frame{};
        const gvfg_status_t st = handle_ ? gvfg_read_frame(handle_, &frame, 200) : GVFG_ESTATE;
        if (st == GVFG_OK)
        {
            if (previewHandle_)
                gvfg_preview_render_frame(previewHandle_, &frame);

            const QImage image = frameToImage(frame);
            gvfg_release_frame(handle_, &frame);

            if (!image.isNull())
                emit frameReady(image);
            continue;
        }

        if (st == GVFG_ETIMEOUT)
            continue;
        if (stopRequested_.load(std::memory_order_acquire) || st == GVFG_ESTATE)
            break;

        emit errorOccurred(QStringLiteral("gvfg_read_frame failed: %1").arg(QString::fromUtf8(gvfg_strerror(st))));
        break;
    }
}
#else
void GvfgSource::onFrame(const gvfg_frame_t *frame, void *user)
{
    auto *self = static_cast<GvfgSource *>(user);
    if (!self || !frame || !frame->data || frame->width <= 0 || frame->height <= 0 || frame->stride <= 0)
        return;

    const QImage image(static_cast<const uchar *>(frame->data),
                       frame->width,
                       frame->height,
                       frame->stride,
                       QImage::Format_ARGB32);
    emit self->frameReady(image.copy());
}

void GvfgSource::onError(gvfg_status_t status, const char *message, void *user)
{
    auto *self = static_cast<GvfgSource *>(user);
    if (!self)
        return;
    const QString detail = QString::fromUtf8(message ? message : "");
    emit self->errorOccurred(QStringLiteral("GVFG error %1: %2")
                                 .arg(static_cast<int>(status))
                                 .arg(detail.isEmpty() ? QString::fromUtf8(gvfg_strerror(status)) : detail));
}
#endif
