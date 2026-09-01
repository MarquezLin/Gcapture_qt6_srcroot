#include "gvfg_source.h"

#include <gvfg_debug.h>

#include <algorithm>
#include <chrono>
#include <cstring>

#ifdef QT6_VIEWER_ENABLE_GVFG_RECORDING
#include "ffmpeg_video_recorder.h"
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

QImage convertFrameToImage(const gvfg_frame_t &frame)
{
    if (frame.pixel_format != GVFG_PIXFMT_YUY2 &&
        frame.pixel_format != GVFG_PIXFMT_Y210)
        return QImage();

    QImage image(frame.width, frame.height, QImage::Format_ARGB32);
    if (image.isNull())
        return QImage();

    return gvfg_gpu_convert_to_bgra8(&frame,
                                     image.bits(),
                                     static_cast<uint64_t>(image.sizeInBytes()),
                                     image.bytesPerLine()) == GVFG_OK
               ? image
               : QImage();
}

QImage frameToImage(const gvfg_frame_t &frame)
{
    if (!frame.data || frame.width <= 0 || frame.height <= 0)
        return QImage();

    return convertFrameToImage(frame);
}

}

GvfgSource::GvfgSource(QObject *parent)
    : QObject(parent)
{
}

GvfgSource::~GvfgSource()
{
    close();
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

bool GvfgSource::open(int deviceIndex, bool zeroCopyEnabled)
{
    const int requestedIndex = std::max(0, deviceIndex);
    if (handle_ && openedDeviceIndex_ == requestedIndex && zeroCopyEnabled_ == zeroCopyEnabled)
        return true;
    close();

    gvfg_status_t st = gvfg_create(&handle_);
    if (st != GVFG_OK || !handle_)
    {
        emit errorOccurred(QStringLiteral("gvfg_create failed: %1").arg(QString::fromUtf8(gvfg_strerror(st))));
        handle_ = nullptr;
        return false;
    }

    st = gvfg_set_channel_zero_copy_enabled(handle_, GVFG_CHANNEL_0, zeroCopyEnabled ? 1 : 0);
    if (st != GVFG_OK)
    {
        emit errorOccurred(QStringLiteral("gvfg_set_channel_zero_copy_enabled failed: %1")
                               .arg(QString::fromUtf8(gvfg_strerror(st))));
        close();
        return false;
    }

    st = gvfg_open_channel(handle_, requestedIndex, GVFG_CHANNEL_0);
    if (st != GVFG_OK)
    {
        emit errorOccurred(QStringLiteral("gvfg_open_channel failed: %1").arg(QString::fromUtf8(gvfg_strerror(st))));
        close();
        return false;
    }

    openedDeviceIndex_ = requestedIndex;
    zeroCopyEnabled_ = zeroCopyEnabled;
    gvfg_signal_status_t status{};
    if (gvfg_get_channel_signal_status(handle_, GVFG_CHANNEL_0, &status) == GVFG_OK)
    {
        std::lock_guard<std::mutex> lock(signalMutex_);
        cachedSignal_ = status;
    }
    emit signalStatusChanged(status.connected != 0);
    return true;
}

void GvfgSource::close()
{
    stop();
    if (handle_)
    {
        gvfg_destroy(handle_);
        handle_ = nullptr;
    }
    openedDeviceIndex_ = -1;
    zeroCopyEnabled_ = false;
    std::lock_guard<std::mutex> lock(signalMutex_);
    cachedSignal_ = {};
}

bool GvfgSource::start(void *previewHwnd, int deviceIndex, int previewBitDepthMode, bool zeroCopyEnabled)
{
    stop();
    if (!open(deviceIndex, zeroCopyEnabled))
        return false;
    if (!setPreview(previewHwnd, previewBitDepthMode))
        return false;
    if (!signalStatus().connected)
    {
        emit errorOccurred(QStringLiteral("GVFG start blocked: no locked input signal."));
        return false;
    }

    const gvfg_status_t st = gvfg_start_channel(handle_, GVFG_CHANNEL_0);
    if (st != GVFG_OK)
    {
        emit errorOccurred(QStringLiteral("gvfg_start failed: %1").arg(QString::fromUtf8(gvfg_strerror(st))));
        setPreview(nullptr, previewBitDepthMode);
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
        gvfg_stop_channel(handle_, GVFG_CHANNEL_0);
    {
        std::lock_guard<std::mutex> lock(recordingMutex_);
        recordingWidth_ = 0;
        recordingHeight_ = 0;
        recordingPixelFormat_ = GVFG_PIXFMT_UNKNOWN;
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

    if (pixelFormat != GVFG_PIXFMT_YUY2 && pixelFormat != GVFG_PIXFMT_Y210)
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
        recordingConfig_.bitrateKbps = bitrateKbps > 0 ? bitrateKbps : 8000;
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
    return captureSnapshotData(timeoutMs, error).image;
}

GvfgSource::Snapshot GvfgSource::captureSnapshotData(int timeoutMs, QString *error)
{
    Snapshot result;
    if (!running_)
    {
        if (error)
            *error = QStringLiteral("GVFG capture is not running.");
        return result;
    }

    {
        std::lock_guard<std::mutex> lock(snapshotMutex_);
        snapshotPending_ = true;
        snapshotComplete_ = false;
        snapshotImage_ = QImage();
        snapshotRawData_.clear();
        snapshotWidth_ = snapshotHeight_ = snapshotStrideBytes_ = 0;
        snapshotPixelFormat_ = GVFG_PIXFMT_UNKNOWN;
        snapshotBitDepth_ = 0;
        snapshotFrameId_ = 0;
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
        return result;
    }

    if ((snapshotImage_.isNull() || snapshotRawData_.isEmpty()) && error)
        *error = snapshotError_.isEmpty() ? QStringLiteral("GVFG snapshot conversion is not supported for the current frame format.") : snapshotError_;
    result.image = snapshotImage_;
    result.rawData = snapshotRawData_;
    result.width = snapshotWidth_;
    result.height = snapshotHeight_;
    result.pixelFormat = snapshotPixelFormat_;
    result.bitDepth = snapshotBitDepth_;
    result.strideBytes = snapshotStrideBytes_;
    result.frameId = snapshotFrameId_;
    return result;
}

gvfg_signal_status_t GvfgSource::signalStatus() const
{
    std::lock_guard<std::mutex> lock(signalMutex_);
    return cachedSignal_;
}

bool GvfgSource::setVideoFormat(gvfg_pixel_format_t format)
{
    if (!handle_ || running_)
        return false;
    const gvfg_status_t st = gvfg_set_channel_video_format(handle_, GVFG_CHANNEL_0, format);
    if (st != GVFG_OK)
    {
        emit errorOccurred(QStringLiteral("gvfg_set_video_format failed: %1").arg(QString::fromUtf8(gvfg_strerror(st))));
        return false;
    }
    return true;
}

void GvfgSource::clearPreview()
{
    if (previewHandle_)
        gvfg_preview_clear(previewHandle_);
}

void GvfgSource::pollEvents()
{
    if (!handle_)
        return;
    gvfg_event_t event{};
    event.struct_size = sizeof(event);
    while (gvfg_poll_channel_event(handle_, GVFG_CHANNEL_0, &event, 0) == GVFG_OK)
    {
        const auto type = static_cast<gvfg_event_type_t>(event.type);
        emit eventOccurred(QStringLiteral("GVFG event %1").arg(gvfgEventName(type)));
        if (type == GVFG_EVENT_SIGNAL_CONNECTED || type == GVFG_EVENT_SIGNAL_DISCONNECTED ||
            type == GVFG_EVENT_FORMAT_CHANGE_BEGIN || type == GVFG_EVENT_STREAM_READY)
        {
            gvfg_signal_status_t status{};
            if (gvfg_get_channel_signal_status(handle_, GVFG_CHANNEL_0, &status) == GVFG_OK)
            {
                bool changed;
                {
                    std::lock_guard<std::mutex> lock(signalMutex_);
                    changed = cachedSignal_.connected != status.connected;
                    cachedSignal_ = status;
                }
                if (changed)
                    emit signalStatusChanged(status.connected != 0);
            }
        }
        event = {};
        event.struct_size = sizeof(event);
    }
}

gvfg_runtime_info_t GvfgSource::runtimeInfo() const
{
    gvfg_runtime_info_t info{};
    if (handle_)
        gvfg_get_channel_runtime_info(handle_, GVFG_CHANNEL_0, &info);
    return info;
}

gvfg_status_t GvfgSource::readRegister(uint32_t offset, uint32_t *outValue) const
{
    if (!handle_)
        return GVFG_ESTATE;
    return gvfg_debug_read_register(handle_, offset, outValue);
}

gvfg_status_t GvfgSource::writeRegister(uint32_t offset, uint32_t value) const
{
    if (!handle_)
        return GVFG_ESTATE;
    return gvfg_debug_write_register(handle_, offset, value);
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
        gvfg_frame_t frame{};
        const gvfg_status_t st = handle_ ? gvfg_read_channel_frame(handle_, GVFG_CHANNEL_0, &frame, 200) : GVFG_ESTATE;
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
                QByteArray rawData;
                const int minRowBytes = frame.width *
                    (frame.pixel_format == GVFG_PIXFMT_Y210 ? 4 : 2);
                if (frameHasRows(frame, minRowBytes))
                {
                    rawData.resize(frame.row_stride_bytes * frame.height);
                    rawData.fill('\0');
                    const char *source = static_cast<const char *>(frame.data);
                    for (int y = 0; y < frame.height; ++y)
                    {
                        const size_t sourceOffset =
                            static_cast<size_t>(y) * static_cast<size_t>(frame.row_stride_bytes);
                        const size_t available = frame.data_size > sourceOffset
                            ? static_cast<size_t>(frame.data_size) - sourceOffset
                            : 0;
                        const size_t copyBytes =
                            std::min(static_cast<size_t>(frame.row_stride_bytes), available);
                        std::memcpy(rawData.data() + qsizetype(y) * frame.row_stride_bytes,
                                    source + sourceOffset, copyBytes);
                    }
                }
                {
                    std::lock_guard<std::mutex> lock(snapshotMutex_);
                    snapshotImage_ = image;
                    snapshotRawData_ = rawData;
                    snapshotWidth_ = frame.width;
                    snapshotHeight_ = frame.height;
                    snapshotPixelFormat_ = frame.pixel_format;
                    snapshotBitDepth_ = frame.bit_depth;
                    snapshotStrideBytes_ = frame.row_stride_bytes;
                    snapshotFrameId_ = frame.frame_id;
                    snapshotError_ = rawData.isEmpty()
                                         ? QStringLiteral("GVFG snapshot frame has an invalid RAW layout.")
                                         : (image.isNull()
                                                ? QStringLiteral("GVFG snapshot preview conversion is not supported for the current frame format.")
                                                : QString());
                    snapshotComplete_ = true;
                }
                snapshotCv_.notify_all();
            }

            writeRecordingFrame(frame);
            const gvfg_status_t releaseStatus = gvfg_release_channel_frame(handle_, GVFG_CHANNEL_0, &frame);
            if (releaseStatus != GVFG_OK)
            {
                emit errorOccurred(QStringLiteral("gvfg_release_frame failed: %1")
                                       .arg(QString::fromUtf8(gvfg_strerror(releaseStatus))));
                break;
            }
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

    if (!frameHasRows(frame, frame.width * ((frame.pixel_format == GVFG_PIXFMT_Y210) ? 4 : 2)) ||
        (frame.width & 1) != 0 || (frame.height & 1) != 0)
    {
        emit errorOccurred(QStringLiteral("GVFG recording stopped: invalid frame layout"));
        stopRecording();
        return;
    }

    QueuedRecordingFrame queued;
    queued.width = frame.width;
    queued.height = frame.height;
    queued.stride = frame.width;
    queued.data.resize(static_cast<size_t>(queued.stride) *
                       static_cast<size_t>(queued.height + queued.height / 2));
    const gvfg_status_t convertStatus = gvfg_gpu_convert_to_nv12(
        &frame, queued.data.data(), static_cast<uint64_t>(queued.data.size()), queued.stride);
    if (convertStatus != GVFG_OK)
    {
        emit errorOccurred(QStringLiteral("GVFG recording stopped: NV12 conversion failed: %1")
                               .arg(QString::fromUtf8(gvfg_strerror(convertStatus))));
        stopRecording();
        return;
    }

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
    {
        std::lock_guard<std::mutex> lock(recordingMutex_);
        config = recordingConfig_;
        width = recordingWidth_;
        height = recordingHeight_;
    }

    FfmpegVideoRecordConfig cfg{};
    cfg.path = config.path;
    cfg.width = width;
    cfg.height = height;
    cfg.fps_num = config.fpsNum;
    cfg.fps_den = config.fpsDen;
    cfg.bitrate_kbps = config.bitrateKbps;
    cfg.input_format = GCAP_FMT_NV12;
    cfg.force_h264 = true;

    FfmpegVideoRecorder recorder;
    std::string error;
    if (!recorder.open(cfg, &error))
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

        FfmpegVideoFrameView view{};
        view.format = GCAP_FMT_NV12;
        view.width = queued.width;
        view.height = queued.height;
        view.data[0] = queued.data.data();
        view.data[1] = queued.data.data() +
                       static_cast<size_t>(queued.stride) * static_cast<size_t>(queued.height);
        view.stride[0] = queued.stride;
        view.stride[1] = queued.stride;
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
