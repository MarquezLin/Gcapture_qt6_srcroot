#include "gvfg_source.h"

#include <algorithm>

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

    gvfg_set_callbacks(handle_, &GvfgSource::onFrame, &GvfgSource::onError, this);

    if (!setPreview(previewHwnd, previewBitDepthMode))
    {
        stop();
        return false;
    }

    st = gvfg_open(handle_, std::max(0, deviceIndex));
    if (st != GVFG_OK)
    {
        emit errorOccurred(QStringLiteral("gvfg_open failed: %1").arg(QString::fromUtf8(gvfg_strerror(st))));
        stop();
        return false;
    }

    st = gvfg_start(handle_);
    if (st != GVFG_OK)
    {
        emit errorOccurred(QStringLiteral("gvfg_start failed: %1").arg(QString::fromUtf8(gvfg_strerror(st))));
        stop();
        return false;
    }

    running_ = true;
    return true;
}

bool GvfgSource::setPreview(void *previewHwnd, int previewBitDepthMode)
{
    if (!handle_)
        return false;

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
}

void GvfgSource::stop()
{
    running_ = false;
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
    if (handle_)
        gvfg_get_preview_info(handle_, &info);
    return info;
}

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
