#include "gvfg_source.h"

#include <chrono>
#include <cstring>

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
    case GVFG_EVENT_SIGNAL_CONNECTED:
        return QStringLiteral("SIGNAL_CONNECTED");
    case GVFG_EVENT_SIGNAL_DISCONNECTED:
        return QStringLiteral("SIGNAL_DISCONNECTED");
    case GVFG_EVENT_STREAM_READY:
        return QStringLiteral("STREAM_READY");
    case GVFG_EVENT_FORMAT_CHANGE_BEGIN:
        return QStringLiteral("FORMAT_CHANGE_BEGIN");
    default:
        return QStringLiteral("UNKNOWN");
    }
}

bool frameHasRows(const gvfg_frame_t &frame, int minRowBytes)
{
    if (!frame.data || frame.width <= 0 || frame.height <= 0 ||
        frame.row_stride_bytes < minRowBytes || minRowBytes <= 0)
        return false;
    const uint64_t required = static_cast<uint64_t>(frame.row_stride_bytes) *
                                  static_cast<uint64_t>(frame.height - 1) +
                              static_cast<uint64_t>(minRowBytes);
    return frame.data_size >= required;
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
    desc.pixel_format = dstFormat;

    gvfg_convert_frame converted = nullptr;
    gvfg_status_t st = gvfg_convert_create_frame(&desc, &converted);
    if (st != GVFG_OK || !converted)
        return QImage();

    QImage image;
    st = gvfg_convert_frame_from_capture(&frame, converted);
    if (st == GVFG_OK)
    {
        gvfg_convert_frame_desc_t convertedDesc{};
        const void *data = nullptr;
        uint64_t dataSize = 0;
        st = gvfg_convert_get_frame_desc(converted, &convertedDesc);
        if (st == GVFG_OK)
            st = gvfg_convert_get_buffer(converted, &data, &dataSize);
        const uint64_t required = convertedDesc.height > 0 && convertedDesc.row_bytes >= minRowBytes
                                      ? static_cast<uint64_t>(convertedDesc.row_bytes) *
                                                static_cast<uint64_t>(convertedDesc.height - 1) +
                                            static_cast<uint64_t>(minRowBytes)
                                      : 0;
        if (st == GVFG_OK && data && required > 0 && dataSize >= required)
        {
            const QImage wrapped(static_cast<const uchar *>(data),
                                 convertedDesc.width,
                                 convertedDesc.height,
                                 convertedDesc.row_bytes,
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
gcap_pixfmt_t gcapPixelFormatForGvfg(int pixelFormat)
{
    switch (pixelFormat)
    {
    case GVFG_PIXFMT_YUY2:
        return GCAP_FMT_YUY2;
    case GVFG_PIXFMT_Y210:
        return GCAP_FMT_Y210;
    default:
        return static_cast<gcap_pixfmt_t>(-1);
    }
}

bool makeFfmpegFrameView(const gvfg_frame_t &frame, int64_t pts, FfmpegVideoFrameView &out)
{
    const gcap_pixfmt_t fmt = gcapPixelFormatForGvfg(frame.pixel_format);
    if (fmt == static_cast<gcap_pixfmt_t>(-1) || !frame.data || frame.width <= 0 || frame.height <= 0)
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
        if (!frameHasRows(frame, rowBytes))
            return false;
        out.data[0] = static_cast<const uint8_t *>(frame.data);
        out.stride[0] = frame.row_stride_bytes;
        return true;
    }
    case GVFG_PIXFMT_Y210:
    {
        const int rowBytes = frame.width * 4;
        if (!frameHasRows(frame, rowBytes))
            return false;
        out.data[0] = static_cast<const uint8_t *>(frame.data);
        out.stride[0] = frame.row_stride_bytes;
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

    st = gvfg_open_channel(handle_, std::max(0, deviceIndex), GVFG_CHANNEL_0);
    if (st != GVFG_OK)
    {
        emit errorOccurred(QStringLiteral("gvfg_open_channel failed: %1").arg(QString::fromUtf8(gvfg_strerror(st))));
        stop();
        return false;
    }

    emit preStartRuntimeInfoReady(runtimeInfo());

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
    {
        std::lock_guard<std::mutex> lock(recordingMutex_);
        recordingWidth_ = 0;
        recordingHeight_ = 0;
        recordingPixelFormat_ = GVFG_PIXFMT_UNKNOWN;
    }
}

bool GvfgSource::startRecording(const QString &path, int fpsNum, int fpsDen, int bitrateKbps, bool useHevc, QString *error)
{
#ifndef QT6_VIEWER_ENABLE_GVFG_RECORDING
    Q_UNUSED(path);
    Q_UNUSED(fpsNum);
    Q_UNUSED(fpsDen);
    Q_UNUSED(bitrateKbps);
    Q_UNUSED(useHevc);
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

    {
        std::lock_guard<std::mutex> lock(recordingMutex_);
        if (recording_)
        {
            if (error)
                *error = QStringLiteral("GVFG recording is already running.");
            return false;
        }
    }

    // Reap a worker that has already finished before publishing a new
    // recording state. Joining after clearing stopRequested can revive an old
    // waiter and race it with the new session.
    if (recordingThread_.joinable())
        recordingThread_.join();

    int width = 0;
    int height = 0;
    int pixelFormat = GVFG_PIXFMT_UNKNOWN;
    {
        std::lock_guard<std::mutex> lock(recordingMutex_);
        if (recording_)
        {
            if (error)
                *error = QStringLiteral("GVFG recording is already running.");
            return false;
        }
        width = recordingWidth_;
        height = recordingHeight_;
        pixelFormat = recordingPixelFormat_;
    }
    if (width <= 0 || height <= 0 || pixelFormat == GVFG_PIXFMT_UNKNOWN)
    {
        if (error)
            *error = QStringLiteral("Wait for the first GVFG frame before starting recording.");
        return false;
    }

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
        recordingConfig_.useHevc = useHevc;
        recording_ = true;
        recordingOpenPending_ = true;
        recordingStopRequested_ = false;
        recordingFrames_ = 0;
        recordingDroppedFrames_ = 0;
        recordingFirstFrameId_ = 0;
        recordingWidth_ = width;
        recordingHeight_ = height;
        recordingPixelFormat_ = pixelFormat;
        recordingQueue_.clear();
    }
    recordingThread_ = std::thread([this]() { recordingLoop(); });
    return true;
#endif
}

void GvfgSource::stopRecording()
{
    {
        std::lock_guard<std::mutex> lock(recordingMutex_);
        recording_ = false;
#ifdef QT6_VIEWER_ENABLE_GVFG_RECORDING
        recordingStopRequested_ = true;
#endif
    }
#ifdef QT6_VIEWER_ENABLE_GVFG_RECORDING
    recordingCv_.notify_all();
    if (recordingThread_.joinable())
        recordingThread_.join();
    {
        std::lock_guard<std::mutex> lock(recordingMutex_);
        recordingOpenPending_ = false;
        recordingStopRequested_ = false;
        recordingQueue_.clear();
    }
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

gvfg_preview_stats_t GvfgSource::previewStats() const
{
    gvfg_preview_stats_t stats{};
    if (previewHandle_)
        gvfg_preview_get_stats(previewHandle_, &stats);
    return stats;
}

void GvfgSource::readLoop()
{
    while (!stopRequested_.load(std::memory_order_acquire))
    {
        gvfg_event_type_t event = GVFG_EVENT_UNKNOWN;
        while (handle_ && gvfg_poll_event(handle_, &event, 0) == GVFG_OK)
        {
            emit errorOccurred(QStringLiteral("GVFG event %1")
                                   .arg(gvfgEventName(event)));
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
            {
                gvfg_preview_frame_t previewFrame{};
                previewFrame.data = frame.data;
                previewFrame.data_size = frame.data_size;
                previewFrame.width = frame.width;
                previewFrame.height = frame.height;
                previewFrame.pixel_format = frame.pixel_format;
                previewFrame.bit_depth = frame.bit_depth;
                previewFrame.row_bytes = frame.row_stride_bytes;
                previewFrame.frame_id = frame.frame_id;
                gvfg_preview_render_frame(previewHandle_, &previewFrame);
            }

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
    {
        std::lock_guard<std::mutex> lock(recordingMutex_);
        if (!recording_ || recordingStopRequested_)
            return;
    }

    if (!frameHasRows(frame, frame.width * ((frame.pixel_format == GVFG_PIXFMT_Y210) ? 4 : 2)))
    {
        emit errorOccurred(QStringLiteral("GVFG recording stopped: invalid frame layout"));
        stopRecording();
        return;
    }

    QueuedRecordingFrame queued;
    queued.width = frame.width;
    queued.height = frame.height;
    queued.pixelFormat = frame.pixel_format;
    queued.stride = frame.row_stride_bytes;
    const size_t rowBytes = static_cast<size_t>(frame.width) *
                            static_cast<size_t>(frame.pixel_format == GVFG_PIXFMT_Y210 ? 4 : 2);
    queued.data.resize(static_cast<size_t>(queued.stride) *
                           static_cast<size_t>(queued.height - 1) +
                       rowBytes);
    std::memcpy(queued.data.data(), frame.data, queued.data.size());

    bool formatChanged = false;
    {
        std::lock_guard<std::mutex> lock(recordingMutex_);
        if (!recording_ || recordingStopRequested_)
            return;
        if (frame.width != recordingWidth_ || frame.height != recordingHeight_ ||
            frame.pixel_format != recordingPixelFormat_)
        {
            recording_ = false;
            recordingStopRequested_ = true;
            recordingQueue_.clear();
            recordingCv_.notify_all();
            formatChanged = true;
        }
        else
        {
            if (recordingFirstFrameId_ == 0)
                recordingFirstFrameId_ = frame.frame_id;
            queued.pts = static_cast<int64_t>(frame.frame_id - recordingFirstFrameId_);

            constexpr size_t kMaxQueuedFrames = 8;
            if (recordingQueue_.size() >= kMaxQueuedFrames)
            {
                recordingQueue_.pop_front();
                ++recordingDroppedFrames_;
            }
            recordingQueue_.push_back(std::move(queued));
        }
    }
    if (formatChanged)
    {
        emit errorOccurred(QStringLiteral("GVFG recording stopped: frame format changed while recording"));
        return;
    }
    recordingCv_.notify_one();
#else
    Q_UNUSED(frame);
#endif
}

void GvfgSource::recordingLoop()
{
#ifdef QT6_VIEWER_ENABLE_GVFG_RECORDING
    RecordingConfig config;
    int width = 0;
    int height = 0;
    int pixelFormat = GVFG_PIXFMT_UNKNOWN;
    {
        std::lock_guard<std::mutex> lock(recordingMutex_);
        config = recordingConfig_;
        width = recordingWidth_;
        height = recordingHeight_;
        pixelFormat = recordingPixelFormat_;
    }

    const gcap_pixfmt_t recordFmt = gcapPixelFormatForGvfg(pixelFormat);
    FfmpegVideoRecordConfig cfg{};
    cfg.path = config.path;
    cfg.width = width;
    cfg.height = height;
    cfg.fps_num = config.fpsNum;
    cfg.fps_den = config.fpsDen;
    cfg.bitrate_kbps = config.bitrateKbps;
    cfg.input_format = recordFmt;
    cfg.force_hevc_main10 = config.useHevc;
    cfg.force_h264 = !config.useHevc;

    FfmpegVideoRecorder recorder;
    std::string error;
    if (recordFmt == static_cast<gcap_pixfmt_t>(-1) || !recorder.open(cfg, &error))
    {
        {
            std::lock_guard<std::mutex> lock(recordingMutex_);
            recording_ = false;
            recordingOpenPending_ = false;
            recordingStopRequested_ = true;
            recordingQueue_.clear();
        }
        emit errorOccurred(QStringLiteral("GVFG recording stopped: %1").arg(QString::fromStdString(error)));
        return;
    }
    {
        std::lock_guard<std::mutex> lock(recordingMutex_);
        recordingOpenPending_ = false;
    }

    while (true)
    {
        QueuedRecordingFrame queued;
        {
            std::unique_lock<std::mutex> lock(recordingMutex_);
            recordingCv_.wait(lock, [this]() {
                return recordingStopRequested_ || !recordingQueue_.empty();
            });
            if (recordingQueue_.empty())
            {
                if (recordingStopRequested_)
                    break;
                continue;
            }
            queued = std::move(recordingQueue_.front());
            recordingQueue_.pop_front();
        }

        if (queued.pixelFormat == GVFG_PIXFMT_Y210)
        {
            // The FPGA DMA payload is Y0, Cr(V), Y1, Cb(U), while standard
            // Y210 (and FFmpeg AV_PIX_FMT_Y210LE) expects Y0, Cb(U), Y1, Cr(V).
            // Normalize in the recording worker so preview/capture stays fast.
            for (int y = 0; y < queued.height; ++y)
            {
                auto *words = reinterpret_cast<uint16_t *>(
                    queued.data.data() + static_cast<size_t>(y) * static_cast<size_t>(queued.stride));
                const int wordCount = queued.width * 2;
                for (int x = 0; x + 3 < wordCount; x += 4)
                    std::swap(words[x + 1], words[x + 3]);
            }
        }

        FfmpegVideoFrameView view{};
        view.format = recordFmt;
        view.width = queued.width;
        view.height = queued.height;
        view.data[0] = queued.data.data();
        view.stride[0] = queued.stride;
        view.pts = queued.pts;
        if (!recorder.writeFrame(view, &error))
        {
            {
                std::lock_guard<std::mutex> lock(recordingMutex_);
                recording_ = false;
                recordingStopRequested_ = true;
                recordingQueue_.clear();
            }
            emit errorOccurred(QStringLiteral("GVFG recording stopped: %1").arg(QString::fromStdString(error)));
            break;
        }
        {
            std::lock_guard<std::mutex> lock(recordingMutex_);
            ++recordingFrames_;
        }
    }
    recorder.close();
#endif
}
