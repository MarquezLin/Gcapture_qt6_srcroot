#include "gvfg_source.h"

#include <chrono>

#ifdef QT6_VIEWER_ENABLE_GVFG_RECORDING
#include "ffmpeg_video_recorder.h"
#endif

#ifdef QT6_VIEWER_ENABLE_GVFG_CONVERT
#include <gvfg_convert.h>
#endif

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

bool getFrameLayout(const gvfg_frame_t &frame, gvfg_frame_layout_t &layout)
{
    layout = {};
    layout.struct_size = sizeof(layout);
    return frame.data && frame.width > 0 && frame.height > 0 && gvfg_get_frame_layout(&frame, &layout) == GVFG_OK;
}

bool layoutPlaneHasRows(const gvfg_frame_layout_t &layout, int plane, int rows, int minRowBytes)
{
    if (plane < 0 || plane >= layout.plane_count || rows <= 0 || minRowBytes <= 0)
        return false;
    if (!layout.plane_data[plane] || layout.plane_stride[plane] < minRowBytes)
        return false;
    const uint64_t required = static_cast<uint64_t>(layout.plane_stride[plane]) * static_cast<uint64_t>(rows - 1) +
                              static_cast<uint64_t>(minRowBytes);
    return layout.plane_size[plane] >= required;
}

#ifdef QT6_VIEWER_ENABLE_GVFG_CONVERT
QImage convertFrameToImage(const gvfg_frame_t &frame)
{
    int dstFormat = 0;
    QImage::Format imageFormat = QImage::Format_Invalid;
    int minRowBytes = 0;
    switch (frame.pixel_format)
    {
    case GVFG_PIXFMT_YUY2:
        dstFormat = GVFG_CONVERT_FMT_BGRA8;
        imageFormat = QImage::Format_ARGB32;
        minRowBytes = frame.width * 4;
        break;
    case GVFG_PIXFMT_Y210:
        dstFormat = GVFG_CONVERT_FMT_RGBA64;
        imageFormat = QImage::Format_RGBA64;
        minRowBytes = frame.width * 8;
        break;
    default:
        return QImage();
    }

    gvfg_convert_frame_desc_t desc{};
    desc.struct_size = sizeof(desc);
    desc.pixel_format = dstFormat;

    gvfg_convert_frame converted = nullptr;
    gvfg_status_t st = gvfg_convert_create_frame(&desc, &converted);
    if (st != GVFG_OK || !converted)
        return QImage();

    QImage image;
    st = gvfg_convert_frame_from_capture(&frame, converted);
    if (st == GVFG_OK)
    {
        gvfg_frame_layout_t layout{};
        layout.struct_size = sizeof(layout);
        st = gvfg_convert_get_layout(converted, &layout);
        if (st == GVFG_OK && layoutPlaneHasRows(layout, 0, frame.height, minRowBytes))
        {
            const QImage wrapped(static_cast<const uchar *>(layout.plane_data[0]),
                                 frame.width,
                                 frame.height,
                                 layout.plane_stride[0],
                                 imageFormat);
            image = wrapped.copy();
        }
    }

    gvfg_convert_destroy_frame(converted);
    return image;
}
#endif

QImage frameToImage(const gvfg_frame_t &frame)
{
    if (!frame.data || frame.width <= 0 || frame.height <= 0)
        return QImage();

#ifdef QT6_VIEWER_ENABLE_GVFG_CONVERT
    return convertFrameToImage(frame);
#else
    return QImage();
#endif
}

#ifdef QT6_VIEWER_ENABLE_GVFG_RECORDING
int gvfgPixelFormatFromName(const char *name)
{
    const QString fmt = QString::fromUtf8(name ? name : "").trimmed().toUpper();
    if (fmt == QStringLiteral("YUY2"))
        return GVFG_PIXFMT_YUY2;
    if (fmt == QStringLiteral("UYVY"))
        return GVFG_PIXFMT_UYVY;
    if (fmt == QStringLiteral("NV12"))
        return GVFG_PIXFMT_NV12;
    if (fmt == QStringLiteral("P010"))
        return GVFG_PIXFMT_P010;
    if (fmt == QStringLiteral("Y210"))
        return GVFG_PIXFMT_Y210;
    if (fmt == QStringLiteral("BGRA8") || fmt == QStringLiteral("BGRA"))
        return GVFG_PIXFMT_BGRA8;
    if (fmt == QStringLiteral("BGRX32"))
        return GVFG_PIXFMT_BGRX32;
    if (fmt == QStringLiteral("RGB24"))
        return GVFG_PIXFMT_RGB24;
    return GVFG_PIXFMT_UNKNOWN;
}

gcap_pixfmt_t gcapPixelFormatForGvfg(int pixelFormat)
{
    switch (pixelFormat)
    {
    case GVFG_PIXFMT_YUY2:
        return GCAP_FMT_YUY2;
    case GVFG_PIXFMT_NV12:
        return GCAP_FMT_NV12;
    case GVFG_PIXFMT_P010:
        return GCAP_FMT_P010;
    case GVFG_PIXFMT_Y210:
        return GCAP_FMT_Y210;
    case GVFG_PIXFMT_BGRA8:
    case GVFG_PIXFMT_BGRX32:
        return GCAP_FMT_ARGB;
    default:
        return static_cast<gcap_pixfmt_t>(-1);
    }
}

bool makeFfmpegFrameView(const gvfg_frame_t &frame, int64_t pts, FfmpegVideoFrameView &out)
{
    const gcap_pixfmt_t fmt = gcapPixelFormatForGvfg(frame.pixel_format);
    if (fmt == static_cast<gcap_pixfmt_t>(-1) || !frame.data || frame.width <= 0 || frame.height <= 0)
        return false;

    gvfg_frame_layout_t layout{};
    if (!getFrameLayout(frame, layout))
        return false;

    out = {};
    out.format = fmt;
    out.width = frame.width;
    out.height = frame.height;
    out.pts = pts;

    switch (frame.pixel_format)
    {
    case GVFG_PIXFMT_YUY2:
    {
        const int rowBytes = frame.width * 2;
        if (!layoutPlaneHasRows(layout, 0, frame.height, rowBytes))
            return false;
        out.data[0] = static_cast<const uint8_t *>(layout.plane_data[0]);
        out.stride[0] = layout.plane_stride[0];
        return true;
    }
    case GVFG_PIXFMT_Y210:
    {
        const int rowBytes = frame.width * 4;
        if (!layoutPlaneHasRows(layout, 0, frame.height, rowBytes))
            return false;
        out.data[0] = static_cast<const uint8_t *>(layout.plane_data[0]);
        out.stride[0] = layout.plane_stride[0];
        return true;
    }
    case GVFG_PIXFMT_NV12:
    {
        if (!layoutPlaneHasRows(layout, 0, frame.height, frame.width) ||
            !layoutPlaneHasRows(layout, 1, (frame.height + 1) / 2, frame.width))
            return false;
        out.data[0] = static_cast<const uint8_t *>(layout.plane_data[0]);
        out.data[1] = static_cast<const uint8_t *>(layout.plane_data[1]);
        out.stride[0] = layout.plane_stride[0];
        out.stride[1] = layout.plane_stride[1];
        return true;
    }
    case GVFG_PIXFMT_P010:
    {
        const int rowBytes = frame.width * 2;
        if (!layoutPlaneHasRows(layout, 0, frame.height, rowBytes) ||
            !layoutPlaneHasRows(layout, 1, (frame.height + 1) / 2, rowBytes))
            return false;
        out.data[0] = static_cast<const uint8_t *>(layout.plane_data[0]);
        out.data[1] = static_cast<const uint8_t *>(layout.plane_data[1]);
        out.stride[0] = layout.plane_stride[0];
        out.stride[1] = layout.plane_stride[1];
        return true;
    }
    case GVFG_PIXFMT_BGRA8:
    case GVFG_PIXFMT_BGRX32:
    {
        const int rowBytes = frame.width * 4;
        if (!layoutPlaneHasRows(layout, 0, frame.height, rowBytes))
            return false;
        out.data[0] = static_cast<const uint8_t *>(layout.plane_data[0]);
        out.stride[0] = layout.plane_stride[0];
        return true;
    }
    default:
        return false;
    }
}
#endif
}

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
    stopRequested_.store(false, std::memory_order_release);
    readThread_ = std::thread([this]() { readLoop(); });
    return true;
}

bool GvfgSource::setPreview(void *previewHwnd, int previewBitDepthMode)
{
    if (!handle_)
        return false;

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
}

void GvfgSource::stop()
{
    running_ = false;
    stopRequested_.store(true, std::memory_order_release);
    snapshotCv_.notify_all();
    if (readThread_.joinable())
        readThread_.join();
    stopRecording();
    if (previewHandle_)
    {
        gvfg_preview_destroy(previewHandle_);
        previewHandle_ = nullptr;
    }
    if (handle_)
    {
        gvfg_stop(handle_);
        gvfg_destroy(handle_);
        handle_ = nullptr;
    }
}

bool GvfgSource::startRecording(const QString &path, int fpsNum, int fpsDen, int bitrateKbps, QString *error)
{
#ifndef QT6_VIEWER_ENABLE_GVFG_RECORDING
    Q_UNUSED(path);
    Q_UNUSED(fpsNum);
    Q_UNUSED(fpsDen);
    Q_UNUSED(bitrateKbps);
    if (error)
        *error = QStringLiteral("GVFG recording is not enabled in this build because FFmpeg headers/libs were not found.");
    return false;
#else
    if (path.isEmpty())
    {
        if (error)
            *error = QStringLiteral("Recording path is empty.");
        return false;
    }

    gvfg_runtime_info_t info{};
    if (!handle_ || gvfg_get_runtime_info(handle_, &info) != GVFG_OK || !info.last_frame.valid)
    {
        if (error)
            *error = QStringLiteral("Wait for the first GVFG frame before starting recording.");
        return false;
    }

    int width = info.last_frame.width;
    int height = info.last_frame.height;
    int pixelFormat = recordingPixelFormat_;
    {
        std::lock_guard<std::mutex> lock(recordingMutex_);
        if (recording_)
        {
            if (error)
                *error = QStringLiteral("GVFG recording is already running.");
            return false;
        }
        width = recordingWidth_ > 0 ? recordingWidth_ : info.last_frame.width;
        height = recordingHeight_ > 0 ? recordingHeight_ : info.last_frame.height;
        pixelFormat = recordingPixelFormat_;
    }
    if (pixelFormat == GVFG_PIXFMT_UNKNOWN)
        pixelFormat = gvfgPixelFormatFromName(info.last_frame.pixel_format);

    const gcap_pixfmt_t recordFmt = gcapPixelFormatForGvfg(pixelFormat);
    if (recordFmt == static_cast<gcap_pixfmt_t>(-1))
    {
        if (error)
            *error = QStringLiteral("GVFG frame format is not supported by the FFmpeg recorder.");
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(recordingMutex_);
        recordingConfig_.path = path.toStdString();
        recordingConfig_.fpsNum = fpsNum > 0 ? fpsNum : 30;
        recordingConfig_.fpsDen = fpsDen > 0 ? fpsDen : 1;
        recordingConfig_.bitrateKbps = bitrateKbps > 0 ? bitrateKbps : ((recordFmt == GCAP_FMT_P010 || recordFmt == GCAP_FMT_Y210) ? 12000 : 8000);
        recording_ = true;
        recordingOpenPending_ = true;
        recordingFrames_ = 0;
        recordingWidth_ = width;
        recordingHeight_ = height;
        recordingPixelFormat_ = pixelFormat;
        recorder_.reset();
    }
    return true;
#endif
}

void GvfgSource::stopRecording()
{
#ifdef QT6_VIEWER_ENABLE_GVFG_RECORDING
    std::unique_ptr<FfmpegVideoRecorder> recorderToClose;
#endif
    {
        std::lock_guard<std::mutex> lock(recordingMutex_);
        recording_ = false;
#ifdef QT6_VIEWER_ENABLE_GVFG_RECORDING
        recordingOpenPending_ = false;
        recorderToClose = std::move(recorder_);
#endif
    }
#ifdef QT6_VIEWER_ENABLE_GVFG_RECORDING
    if (recorderToClose)
        recorderToClose->close();
#endif
}

bool GvfgSource::isRecording() const
{
    std::lock_guard<std::mutex> lock(recordingMutex_);
    return recording_;
}

uint64_t GvfgSource::recordingFrames() const
{
    std::lock_guard<std::mutex> lock(recordingMutex_);
    return recordingFrames_;
}

QImage GvfgSource::captureSnapshot(int timeoutMs, QString *error)
{
    if (!running_)
    {
        if (error)
            *error = QStringLiteral("GVFG capture is not running.");
        return QImage();
    }

    {
        std::lock_guard<std::mutex> lock(snapshotMutex_);
        snapshotPending_ = true;
        snapshotComplete_ = false;
        snapshotImage_ = QImage();
        snapshotError_.clear();
    }

    std::unique_lock<std::mutex> lock(snapshotMutex_);
    const bool done = snapshotCv_.wait_for(lock,
                                           std::chrono::milliseconds(timeoutMs > 0 ? timeoutMs : 2000),
                                           [this]() { return snapshotComplete_ || !running_; });
    if (!done)
    {
        snapshotPending_ = false;
        if (error)
            *error = QStringLiteral("Timed out waiting for a GVFG frame to snapshot.");
        return QImage();
    }

    if (snapshotImage_.isNull() && error)
        *error = snapshotError_.isEmpty() ? QStringLiteral("GVFG snapshot conversion is not supported for the current frame format.") : snapshotError_;
    return snapshotImage_;
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
    if (previewHandle_)
        gvfg_preview_get_info(previewHandle_, &info);
    return info;
}

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
            {
                std::lock_guard<std::mutex> lock(recordingMutex_);
                if (!recording_)
                {
                    recordingWidth_ = frame.width;
                    recordingHeight_ = frame.height;
                    recordingPixelFormat_ = frame.pixel_format;
                }
            }

            if (previewHandle_)
                gvfg_preview_render_frame(previewHandle_, &frame);

            bool snapshotRequested = false;
            {
                std::lock_guard<std::mutex> lock(snapshotMutex_);
                if (snapshotPending_)
                {
                    snapshotPending_ = false;
                    snapshotRequested = true;
                }
            }
            if (snapshotRequested)
            {
                const QImage image = frameToImage(frame);
                {
                    std::lock_guard<std::mutex> lock(snapshotMutex_);
                    snapshotImage_ = image;
                    snapshotError_ = image.isNull()
                                         ? QStringLiteral("GVFG snapshot conversion is not supported for the current frame format.")
                                         : QString();
                    snapshotComplete_ = true;
                }
                snapshotCv_.notify_all();
            }

            writeRecordingFrame(frame);
            gvfg_release_frame(handle_, &frame);
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

void GvfgSource::writeRecordingFrame(const gvfg_frame_t &frame)
{
#ifdef QT6_VIEWER_ENABLE_GVFG_RECORDING
    std::string error;
    bool failed = false;
    std::unique_ptr<FfmpegVideoRecorder> recorderToClose;
    {
        std::lock_guard<std::mutex> lock(recordingMutex_);
        if (!recording_ || (!recorder_ && !recordingOpenPending_))
            return;

        if (frame.width != recordingWidth_ || frame.height != recordingHeight_ || frame.pixel_format != recordingPixelFormat_)
        {
            error = "GVFG frame format changed while recording";
            failed = true;
        }
        else
        {
            if (recordingOpenPending_)
            {
                const gcap_pixfmt_t recordFmt = gcapPixelFormatForGvfg(recordingPixelFormat_);
                if (recordFmt == static_cast<gcap_pixfmt_t>(-1))
                {
                    error = "GVFG frame format is not supported by the FFmpeg recorder";
                    failed = true;
                }
                else
                {
                    FfmpegVideoRecordConfig cfg{};
                    cfg.path = recordingConfig_.path;
                    cfg.width = recordingWidth_;
                    cfg.height = recordingHeight_;
                    cfg.fps_num = recordingConfig_.fpsNum;
                    cfg.fps_den = recordingConfig_.fpsDen;
                    cfg.bitrate_kbps = recordingConfig_.bitrateKbps;
                    cfg.input_format = recordFmt;
                    cfg.force_hevc_main10 = (recordFmt == GCAP_FMT_P010 || recordFmt == GCAP_FMT_Y210);

                    auto recorder = std::make_unique<FfmpegVideoRecorder>();
                    if (!recorder->open(cfg, &error))
                    {
                        failed = true;
                    }
                    else
                    {
                        recorder_ = std::move(recorder);
                        recordingOpenPending_ = false;
                    }
                }
            }

            if (!failed)
            {
                FfmpegVideoFrameView view{};
                if (!makeFfmpegFrameView(frame, static_cast<int64_t>(recordingFrames_), view))
                {
                    error = "GVFG frame format is not supported by the FFmpeg recorder";
                    failed = true;
                }
                else if (!recorder_ || !recorder_->writeFrame(view, &error))
                {
                    failed = true;
                }
                else
                {
                    ++recordingFrames_;
                }
            }
        }

        if (failed)
        {
            recording_ = false;
            recordingOpenPending_ = false;
            recorderToClose = std::move(recorder_);
        }
    }

    if (recorderToClose)
        recorderToClose->close();

    if (failed)
        emit errorOccurred(QStringLiteral("GVFG recording stopped: %1").arg(QString::fromStdString(error)));
#else
    Q_UNUSED(frame);
#endif
}
