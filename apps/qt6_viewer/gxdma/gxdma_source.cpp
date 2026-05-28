#include "gxdma_source.h"

#include <algorithm>

GxdmaSource::GxdmaSource(QObject *parent)
    : QObject(parent)
{
}

GxdmaSource::~GxdmaSource()
{
    stop();
}

QStringList GxdmaSource::enumerateDevices()
{
    gxdma_device_info_t devices[GXDMA_MAX_DEVICES] = {};
    const int n = gxdma_enumerate_devices(devices, GXDMA_MAX_DEVICES);
    QStringList names;
    for (int i = 0; i < n; ++i)
    {
        const QString name = QString::fromUtf8(devices[i].name);
        names << (name.isEmpty() ? QStringLiteral("XDMA Capture") : name);
    }
    return names;
}

bool GxdmaSource::start(void *previewHwnd, int deviceIndex, int previewBitDepthMode)
{
    stop();

    gxdma_status_t st = gxdma_create(&handle_);
    if (st != GXDMA_OK || !handle_)
    {
        emit errorOccurred(QStringLiteral("gxdma_create failed: %1").arg(QString::fromUtf8(gxdma_strerror(st))));
        handle_ = nullptr;
        return false;
    }

    gxdma_set_callbacks(handle_, &GxdmaSource::onFrame, &GxdmaSource::onError, this);

    if (!setPreview(previewHwnd, previewBitDepthMode))
    {
        stop();
        return false;
    }

    st = gxdma_open(handle_, std::max(0, deviceIndex));
    if (st != GXDMA_OK)
    {
        emit errorOccurred(QStringLiteral("gxdma_open failed: %1").arg(QString::fromUtf8(gxdma_strerror(st))));
        stop();
        return false;
    }

    st = gxdma_start(handle_);
    if (st != GXDMA_OK)
    {
        emit errorOccurred(QStringLiteral("gxdma_start failed: %1").arg(QString::fromUtf8(gxdma_strerror(st))));
        stop();
        return false;
    }

    running_ = true;
    return true;
}

bool GxdmaSource::setPreview(void *previewHwnd, int previewBitDepthMode)
{
    if (!handle_)
        return false;

    gxdma_preview_desc_t preview{};
    preview.hwnd = previewHwnd;
    preview.enable_preview = previewHwnd ? 1 : 0;
    preview.swapchain_bitdepth = previewBitDepthMode;
    const gxdma_status_t st = gxdma_set_preview(handle_, &preview);
    if (st != GXDMA_OK)
    {
        emit errorOccurred(QStringLiteral("gxdma_set_preview failed: %1").arg(QString::fromUtf8(gxdma_strerror(st))));
        return false;
    }
    return true;
}

void GxdmaSource::stop()
{
    running_ = false;
    if (handle_)
    {
        gxdma_stop(handle_);
        gxdma_destroy(handle_);
        handle_ = nullptr;
    }
}

gxdma_signal_status_t GxdmaSource::signalStatus() const
{
    gxdma_signal_status_t status{};
    if (handle_)
        gxdma_get_signal_status(handle_, &status);
    return status;
}

gxdma_runtime_info_t GxdmaSource::runtimeInfo() const
{
    gxdma_runtime_info_t info{};
    if (handle_)
        gxdma_get_runtime_info(handle_, &info);
    return info;
}

gxdma_preview_info_t GxdmaSource::previewInfo() const
{
    gxdma_preview_info_t info{};
    if (handle_)
        gxdma_get_preview_info(handle_, &info);
    return info;
}

void GxdmaSource::onFrame(const gxdma_frame_t *frame, void *user)
{
    auto *self = static_cast<GxdmaSource *>(user);
    if (!self || !frame || !frame->data || frame->width <= 0 || frame->height <= 0 || frame->stride <= 0)
        return;

    const QImage image(static_cast<const uchar *>(frame->data),
                       frame->width,
                       frame->height,
                       frame->stride,
                       QImage::Format_ARGB32);
    emit self->frameReady(image.copy());
}

void GxdmaSource::onError(gxdma_status_t status, const char *message, void *user)
{
    auto *self = static_cast<GxdmaSource *>(user);
    if (!self)
        return;
    const QString detail = QString::fromUtf8(message ? message : "");
    emit self->errorOccurred(QStringLiteral("GXDMA error %1: %2")
                                 .arg(static_cast<int>(status))
                                 .arg(detail.isEmpty() ? QString::fromUtf8(gxdma_strerror(status)) : detail));
}
