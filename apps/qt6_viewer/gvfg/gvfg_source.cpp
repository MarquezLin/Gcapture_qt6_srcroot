#include "gvfg_source.h"

#include <gvfg_debug.h>
#include <gvfg_audio_playback.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

namespace
{
QString gvfgEventName(gvfg_event_type_t type)
{
    switch (type)
    {
    case GVFG_EVENT_VIDEO_FORMAT_CHANGED:
        return QStringLiteral("VIDEO_FORMAT_CHANGED");
    case GVFG_EVENT_VIDEO_INPUT_PLUGIN:
        return QStringLiteral("VIDEO_INPUT_PLUGIN");
    case GVFG_EVENT_VIDEO_INPUT_UNPLUG:
        return QStringLiteral("VIDEO_INPUT_UNPLUG");
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

QString gvfgErrorMessage(const char *apiName, gvfg_status_t status,
                         gvfg_handle handle, int channel = GVFG_CHANNEL_0);

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
        emit errorOccurred(gvfgErrorMessage("gvfg_create", st, handle_));
        handle_ = nullptr;
        return false;
    }

    st = gvfg_set_channel_zero_copy_enabled(handle_, GVFG_CHANNEL_0, zeroCopyEnabled ? 1 : 0);
    if (st != GVFG_OK)
    {
        emit errorOccurred(gvfgErrorMessage(
            "gvfg_set_channel_zero_copy_enabled", st, handle_));
        close();
        return false;
    }

    st = gvfg_open_channel(handle_, requestedIndex, GVFG_CHANNEL_0);
    if (st != GVFG_OK)
    {
        emit errorOccurred(gvfgErrorMessage("gvfg_open_channel", st, handle_));
        close();
        return false;
    }

    openedDeviceIndex_ = requestedIndex;
    zeroCopyEnabled_ = zeroCopyEnabled;
    refreshSignalStatus();
    return true;
}

QString gvfgErrorMessage(const char *apiName, gvfg_status_t status,
                         gvfg_handle handle, int channel)
{
    QString message = QStringLiteral("%1 failed: %2")
                          .arg(QString::fromLatin1(apiName),
                               QString::fromUtf8(gvfg_strerror(status)));
    if (status < 0 && handle)
    {
        char detail[512] = {};
        if (gvfg_get_channel_last_sdk_error_detail(
                handle, channel, detail, sizeof(detail)) == GVFG_OK &&
            detail[0] != '\0')
            message += QStringLiteral(" | %1").arg(QString::fromUtf8(detail));
    }
    return message;
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

bool GvfgSource::start(void *previewHwnd, int deviceIndex, int previewBitDepthMode,
                       bool zeroCopyEnabled, bool audioEnabled)
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

    gvfg_status_t st = gvfg_set_channel_audio_enabled(
        handle_, GVFG_CHANNEL_0, audioEnabled ? 1 : 0);
    if (st != GVFG_OK)
    {
        emit errorOccurred(gvfgErrorMessage(
            "gvfg_set_channel_audio_enabled", st, handle_));
        setPreview(nullptr, previewBitDepthMode);
        return false;
    }

    audioFormat_ = {};
    if (audioEnabled)
    {
        st = gvfg_get_channel_audio_format(handle_, GVFG_CHANNEL_0, &audioFormat_);
        if (st != GVFG_OK)
        {
            emit errorOccurred(gvfgErrorMessage(
                "gvfg_get_channel_audio_format", st, handle_));
            setPreview(nullptr, previewBitDepthMode);
            return false;
        }
        if (audioFormat_.sample_rate == 0 || audioFormat_.channels == 0 ||
            (audioFormat_.bits_per_sample != 8 &&
             audioFormat_.bits_per_sample != 16 &&
             audioFormat_.bits_per_sample != 32))
        {
            emit errorOccurred(QStringLiteral("GVFG audio format is not supported by the WASAPI playback helper: %1 Hz, %2 ch, %3-bit")
                                   .arg(audioFormat_.sample_rate)
                                   .arg(audioFormat_.channels)
                                   .arg(audioFormat_.bits_per_sample));
            setPreview(nullptr, previewBitDepthMode);
            return false;
        }
    }

    st = gvfg_start_channel(handle_, GVFG_CHANNEL_0);
    if (st != GVFG_OK)
    {
        emit errorOccurred(gvfgErrorMessage("gvfg_start_channel", st, handle_));
        setPreview(nullptr, previewBitDepthMode);
        return false;
    }

    running_ = true;
    audioEnabled_ = audioEnabled;
    latestVideoTimestampNs_.store(0, std::memory_order_release);
    signalConnected_.store(true, std::memory_order_release);
    stopRequested_.store(false, std::memory_order_release);
    readThread_ = std::thread([this]() { readLoop(); });
    if (audioEnabled_)
    {
        audioThread_ = std::thread([this]() { audioReadLoop(); });
    }
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

void GvfgSource::setAudioVolume(float volume)
{
    audioVolume_.store(std::clamp(volume, 0.0f, 2.0f), std::memory_order_release);
}

void GvfgSource::stop()
{
    running_ = false;
    stopRequested_.store(true, std::memory_order_release);
    signalCv_.notify_all();
    snapshotCv_.notify_all();
    if (readThread_.joinable())
        readThread_.join();
    if (audioThread_.joinable())
        audioThread_.join();
    {
        std::lock_guard<std::mutex> lock(audioPlaybackMutex_);
        gvfg_audio_player_destroy(audioPlayer_);
        audioPlayer_ = nullptr;
    }
    stopRecording();
    if (previewHandle_)
    {
        gvfg_preview_destroy(previewHandle_);
        previewHandle_ = nullptr;
    }
    if (handle_)
        gvfg_stop_channel(handle_, GVFG_CHANNEL_0);
    audioEnabled_ = false;
    audioFormat_ = {};
    {
        std::lock_guard<std::mutex> lock(recordingMutex_);
        recordingWidth_ = 0;
        recordingHeight_ = 0;
        recordingPixelFormat_ = GVFG_PIXFMT_UNKNOWN;
    }
}

void GvfgSource::audioReadLoop()
{
    while (!stopRequested_.load(std::memory_order_acquire))
    {
        {
            std::unique_lock<std::mutex> lock(signalMutex_);
            signalCv_.wait(lock, [this]() {
                return stopRequested_.load(std::memory_order_acquire) ||
                       signalConnected_.load(std::memory_order_acquire);
            });
        }
        if (stopRequested_.load(std::memory_order_acquire))
            break;

        gvfg_audio_frame_t frame{};
        const gvfg_status_t st = gvfg_read_channel_audio_frame(
            handle_, GVFG_CHANNEL_0, &frame, 200);
        if (st == GVFG_ETIMEOUT)
            continue;
        if (st != GVFG_OK)
        {
            if (!stopRequested_.load(std::memory_order_acquire))
                emit errorOccurred(gvfgErrorMessage(
                    "gvfg_read_channel_audio_frame", st, handle_));
            break;
        }

        const auto *begin = static_cast<const uint8_t *>(frame.data);
        std::vector<uint8_t> pcm(begin, begin + frame.data_size);

        const gvfg_status_t releaseStatus =
            gvfg_release_channel_audio_frame(handle_, GVFG_CHANNEL_0, &frame);
        if (releaseStatus != GVFG_OK)
        {
            if (!stopRequested_.load(std::memory_order_acquire))
                emit errorOccurred(gvfgErrorMessage(
                    "gvfg_release_channel_audio_frame", releaseStatus, handle_));
            break;
        }

#ifdef QT6_VIEWER_ENABLE_GVFG_RECORDING
        {
            std::lock_guard<std::mutex> lock(recordingMutex_);
            if (recording_ && !recordingStopRequested_)
            {
                QueuedRecordingAudio queued;
                queued.data = pcm;
                queued.timestampNs = frame.timestamp_ns;
                constexpr size_t kMaxQueuedAudioPackets = 64;
                if (recordingAudioQueue_.size() >= kMaxQueuedAudioPackets)
                    recordingAudioQueue_.pop_front();
                recordingAudioQueue_.push_back(std::move(queued));
                recordingCv_.notify_one();
            }
        }
#endif

        QString playbackError;
        bool playbackStarted = false;
        {
            std::lock_guard<std::mutex> lock(audioPlaybackMutex_);
            if (!audioPlayer_ && std::chrono::steady_clock::now() >= audioPlaybackRetry_)
            {
                gvfg_audio_player next = nullptr;
                gvfg_audio_playback_status_t status = gvfg_audio_player_create(&next);
                gvfg_audio_playback_format_t outputFormat{};
                outputFormat.sample_rate = audioFormat_.sample_rate;
                outputFormat.channels = audioFormat_.channels;
                outputFormat.bits_per_sample = audioFormat_.bits_per_sample;
                if (status == GVFG_AUDIO_PLAYBACK_OK)
                    status = gvfg_audio_player_start(next, &outputFormat);
                if (status == GVFG_AUDIO_PLAYBACK_OK)
                {
                    audioPlayer_ = next;
                    gvfg_audio_player_set_volume(audioPlayer_, audioVolume_.load(std::memory_order_acquire));
                    const uint64_t videoTimestampNs = latestVideoTimestampNs_.load(std::memory_order_acquire);
                    if (videoTimestampNs != 0)
                        gvfg_audio_player_update_video_timestamp(audioPlayer_, videoTimestampNs);
                    playbackStarted = true;
                }
                else
                {
                    playbackError = QString::fromUtf8(gvfg_audio_player_strerror(status));
                    gvfg_audio_player_destroy(next);
                    audioPlaybackRetry_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
                }
            }
            if (audioPlayer_)
            {
                gvfg_audio_player_set_volume(audioPlayer_, audioVolume_.load(std::memory_order_acquire));
                const auto status = gvfg_audio_player_write_timed(
                    audioPlayer_, pcm.data(), static_cast<uint32_t>(pcm.size()), frame.timestamp_ns);
                if (status != GVFG_AUDIO_PLAYBACK_OK && status != GVFG_AUDIO_PLAYBACK_EQUEUE_FULL)
                {
                    playbackError = QString::fromUtf8(gvfg_audio_player_strerror(status));
                    gvfg_audio_player failed = audioPlayer_;
                    audioPlayer_ = nullptr;
                    gvfg_audio_player_destroy(failed);
                    audioPlaybackRetry_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
                }
            }
        }
        if (playbackStarted)
            emit eventOccurred(QStringLiteral("Audio playback started via GVFG WASAPI helper: %1 Hz, %2 ch, %3-bit PCM")
                                   .arg(audioFormat_.sample_rate).arg(audioFormat_.channels).arg(audioFormat_.bits_per_sample));
        if (!playbackError.isEmpty())
            emit errorOccurred(QStringLiteral("GVFG audio helper unavailable: %1; retrying.").arg(playbackError));
    }
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
        emit errorOccurred(gvfgErrorMessage(
            "gvfg_set_channel_video_format", st, handle_));
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
    gvfg_event_type_t eventType = GVFG_EVENT_UNKNOWN;
    while (gvfg_poll_channel_event(handle_, GVFG_CHANNEL_0, &eventType, 0) == GVFG_OK)
    {
        emit eventOccurred(QStringLiteral("GVFG event %1").arg(gvfgEventName(eventType)));
        if (eventType == GVFG_EVENT_VIDEO_INPUT_UNPLUG)
        {
            bool changed;
            {
                std::lock_guard<std::mutex> lock(signalMutex_);
                changed = cachedSignal_.connected != 0 || cachedSignal_.width != 0 ||
                          cachedSignal_.height != 0;
                cachedSignal_ = {};
                cachedSignal_.channel = GVFG_CHANNEL_0;
            }
            if (changed)
                emit signalStatusChanged(false);
        }
        else if (eventType == GVFG_EVENT_VIDEO_FORMAT_CHANGED ||
                 eventType == GVFG_EVENT_VIDEO_INPUT_PLUGIN)
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
        if (eventType == GVFG_EVENT_VIDEO_INPUT_UNPLUG)
        {
            signalConnected_.store(false, std::memory_order_release);
            latestVideoTimestampNs_.store(0, std::memory_order_release);
            {
                std::lock_guard<std::mutex> lock(audioPlaybackMutex_);
                if (audioPlayer_)
                    gvfg_audio_player_reset_timeline(audioPlayer_);
            }
        }
        else if (eventType == GVFG_EVENT_VIDEO_FORMAT_CHANGED)
        {
            latestVideoTimestampNs_.store(0, std::memory_order_release);
            {
                std::lock_guard<std::mutex> lock(audioPlaybackMutex_);
                if (audioPlayer_)
                    gvfg_audio_player_reset_timeline(audioPlayer_);
            }
        }
        else if (eventType == GVFG_EVENT_VIDEO_INPUT_PLUGIN)
        {
            signalConnected_.store(true, std::memory_order_release);
            latestVideoTimestampNs_.store(0, std::memory_order_release);
            {
                std::lock_guard<std::mutex> lock(audioPlaybackMutex_);
                if (audioPlayer_)
                    gvfg_audio_player_reset_timeline(audioPlayer_);
            }
            signalCv_.notify_all();
        }
        eventType = GVFG_EVENT_UNKNOWN;
    }
}

bool GvfgSource::refreshSignalStatus()
{
    if (!handle_)
        return false;

    gvfg_signal_status_t status{};
    if (gvfg_get_channel_signal_status(handle_, GVFG_CHANNEL_0, &status) != GVFG_OK)
        return false;

    bool connectedChanged;
    {
        std::lock_guard<std::mutex> lock(signalMutex_);
        connectedChanged = cachedSignal_.connected != status.connected;
        cachedSignal_ = status;
    }
    if (connectedChanged)
        emit signalStatusChanged(status.connected != 0);
    return true;
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
        {
            std::unique_lock<std::mutex> lock(signalMutex_);
            signalCv_.wait(lock, [this]() {
                return stopRequested_.load(std::memory_order_acquire) ||
                       signalConnected_.load(std::memory_order_acquire);
            });
        }
        if (stopRequested_.load(std::memory_order_acquire))
            break;

        gvfg_frame_t frame{};
        const gvfg_status_t st = handle_ ? gvfg_read_channel_frame(handle_, GVFG_CHANNEL_0, &frame, 200) : GVFG_ESTATE;
        if (st == GVFG_OK)
        {
            {
                std::lock_guard<std::mutex> lock(signalMutex_);
                cachedSignal_.connected = 1;
                cachedSignal_.channel = GVFG_CHANNEL_0;
                cachedSignal_.width = frame.width;
                cachedSignal_.height = frame.height;
                cachedSignal_.pixel_format = frame.pixel_format;
                cachedSignal_.bit_depth = frame.bit_depth;
            }
            latestVideoTimestampNs_.store(frame.timestamp_ns, std::memory_order_release);
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
                emit errorOccurred(gvfgErrorMessage(
                    "gvfg_release_channel_frame", releaseStatus, handle_));
                break;
            }
            {
                std::lock_guard<std::mutex> lock(audioPlaybackMutex_);
                if (audioPlayer_)
                    gvfg_audio_player_update_video_timestamp(audioPlayer_, frame.timestamp_ns);
            }
            continue;
        }

        if (st == GVFG_ETIMEOUT)
            continue;
        if (stopRequested_.load(std::memory_order_acquire) || st == GVFG_ESTATE)
            break;

        emit errorOccurred(gvfgErrorMessage(
            "gvfg_read_channel_frame", st, handle_));
        break;
    }
}
