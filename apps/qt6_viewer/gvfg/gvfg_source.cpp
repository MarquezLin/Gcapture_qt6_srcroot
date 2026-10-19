#include "gvfg_source.h"

#include <gvfg_debug.h>
#include <gvfg_audio_playback.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

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

QString osdSizeError(const GvfgSource::OsdPlacement &placement)
{
    return QStringLiteral("OSD text box %1 x %2 is larger than frame %3 x %4. Reduce the font size or shorten the text.")
        .arg(placement.textSize.width()).arg(placement.textSize.height())
        .arg(placement.frameSize.width()).arg(placement.frameSize.height());
}

}

QString gvfgErrorMessage(const char *apiName, gvfg_status_t status,
                         gvfg_handle handle, int channel = GVFG_CHANNEL_0);

GvfgSource::GvfgSource(QObject *parent)
    : QObject(parent)
{
    // Frame geometry may be reported by readLoop(); settings and SDK updates
    // stay on this object's GUI thread, never on the acquisition thread.
    connect(this, &GvfgSource::osdFrameSizeChanged,
            this, &GvfgSource::adjustOsdForFrameSize, Qt::QueuedConnection);
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

bool GvfgSource::osdPlacement(const QString &text, uint32_t fontSizePx,
                              OsdPlacement *placement, QString *error) const
{
    if (error)
        error->clear();
    if (!placement)
        return false;
    *placement = {};
    if (text.isEmpty() || text.size() > GVFG_OSD_MAX_TEXT_CHARACTERS || fontSizePx == 0)
    {
        if (error)
            *error = QStringLiteral("OSD requires 1 to %1 printable ASCII characters and a non-zero font size.")
                         .arg(GVFG_OSD_MAX_TEXT_CHARACTERS);
        return false;
    }
    for (const QChar character : text)
    {
        if (character.unicode() < 0x20 || character.unicode() > 0x7e)
        {
            if (error)
                *error = QStringLiteral("OSD text must contain only printable English ASCII characters.");
            return false;
        }
    }

    const QByteArray ascii = text.toLatin1();
    uint32_t width = 0;
    uint32_t height = 0;
    const gvfg_status_t status = gvfg_measure_osd_text(
        ascii.constData(), static_cast<uint32_t>(ascii.size()), fontSizePx, &width, &height);
    if (status != GVFG_OK || width > static_cast<uint32_t>((std::numeric_limits<int>::max)()) ||
        height > static_cast<uint32_t>((std::numeric_limits<int>::max)()))
    {
        if (error)
            *error = status == GVFG_OK ? QStringLiteral("OSD text dimensions are too large.")
                                      : gvfgErrorMessage("gvfg_measure_osd_text", status, nullptr);
        return false;
    }
    placement->textSize = QSize(static_cast<int>(width), static_cast<int>(height));
    const auto signal = signalStatus();
    if (signal.connected && signal.width > 0 && signal.height > 0)
        placement->frameSize = QSize(signal.width, signal.height);
    return true;
}

bool GvfgSource::normalizeOsdSettings(OsdSettings &settings, OsdPlacement *placement,
                                     QString *error) const
{
    if (!osdPlacement(QString::fromLatin1(settings.text), settings.fontSizePx, placement, error))
        return false;
    settings.textWidth = static_cast<uint32_t>(placement->textSize.width());
    settings.textHeight = static_cast<uint32_t>(placement->textSize.height());
    settings.x = std::max(0, settings.x);
    settings.y = std::max(0, settings.y);
    if (placement->frameKnown() && placement->fits())
    {
        settings.x = std::min(settings.x, placement->frameSize.width() - placement->textSize.width());
        settings.y = std::min(settings.y, placement->frameSize.height() - placement->textSize.height());
    }
    return true;
}

bool GvfgSource::setOsd(const QString &text, int x, int y,
                        uint32_t fontSizePx, uint32_t colorRgb, QString *error)
{
    OsdSettings settings{text.toLatin1(), x, y, fontSizePx, colorRgb & 0x00ffffffu};
    // Validate the original QString before converting: Latin-1 replacement of
    // a non-ASCII character must not accidentally turn it into valid OSD text.
    OsdPlacement placement;
    if (!osdPlacement(text, fontSizePx, &placement, error))
        return false;
    if (!placement.fits())
    {
        if (error)
            *error = osdSizeError(placement);
        return false;
    }
    settings.textWidth = static_cast<uint32_t>(placement.textSize.width());
    settings.textHeight = static_cast<uint32_t>(placement.textSize.height());
    settings.x = std::max(0, settings.x);
    settings.y = std::max(0, settings.y);
    if (placement.frameKnown())
    {
        settings.x = std::min(settings.x, placement.frameSize.width() - placement.textSize.width());
        settings.y = std::min(settings.y, placement.frameSize.height() - placement.textSize.height());
    }
    if (handle_)
    {
        const gvfg_status_t status = applyOsdSettings(settings);
        if (status != GVFG_OK)
        {
            if (error)
                *error = gvfgErrorMessage("gvfg_set_channel_osd", status, handle_);
            return false;
        }
    }
    osdSettings_ = std::move(settings);
    osdSizeWarning_.clear();
    return true;
}

void GvfgSource::reportOsdSizeWarning(const OsdPlacement &placement)
{
    const QString warning = placement.fits() ? QString()
        : osdSizeError(placement) + QStringLiteral(" OSD is temporarily skipped; capture continues.");
    if (warning == osdSizeWarning_)
        return;
    osdSizeWarning_ = warning;
    if (!warning.isEmpty())
        emit errorOccurred(warning);
}

void GvfgSource::adjustOsdForFrameSize()
{
    if (!handle_ || !osdSettings_)
        return;
    OsdSettings settings = *osdSettings_;
    OsdPlacement placement;
    QString error;
    if (!normalizeOsdSettings(settings, &placement, &error))
    {
        emit errorOccurred(error);
        return;
    }
    if (!placement.frameKnown())
        return;
    reportOsdSizeWarning(placement);
    // The SDK also clamps at surface rebuild time, covering the very first
    // frame after a resolution change, before this queued handler can run.
    if (settings.x == osdSettings_->x && settings.y == osdSettings_->y)
        return;
    const gvfg_status_t status = applyOsdSettings(settings);
    if (status != GVFG_OK)
    {
        emit errorOccurred(gvfgErrorMessage("gvfg_set_channel_osd", status, handle_));
        return;
    }
    osdSettings_ = std::move(settings);
}

gvfg_status_t GvfgSource::applyOsdSettings(const OsdSettings &settings)
{
    gvfg_channel_osd_config_t config{};
    config.text = settings.text.constData();
    config.text_length = static_cast<uint32_t>(settings.text.size());
    config.x = settings.x;
    config.y = settings.y;
    config.font_size_px = settings.fontSizePx;
    config.color_rgb = settings.colorRgb;
    osdDiagnosticEpoch_.fetch_add(1, std::memory_order_acq_rel);
    const gvfg_status_t status = gvfg_set_channel_osd(handle_, GVFG_CHANNEL_0, &config);
    if (status == GVFG_OK)
    {
        osdDiagnosticEnabled_.store(true, std::memory_order_relaxed);
        osdDiagnosticVisible_.store(std::any_of(settings.text.cbegin(), settings.text.cend(),
            [](char character) { return character != ' '; }), std::memory_order_relaxed);
        osdDiagnosticTextWidth_.store(settings.textWidth, std::memory_order_relaxed);
        osdDiagnosticTextHeight_.store(settings.textHeight, std::memory_order_relaxed);
    }
    osdDiagnosticEpoch_.fetch_add(1, std::memory_order_acq_rel);
    return status;
}

bool GvfgSource::clearOsd(QString *error)
{
    if (error)
        error->clear();
    osdDiagnosticEpoch_.fetch_add(1, std::memory_order_acq_rel);
    const gvfg_status_t status = handle_ ? gvfg_clear_channel_osd(handle_, GVFG_CHANNEL_0) : GVFG_OK;
    if (status == GVFG_OK)
        osdDiagnosticEnabled_.store(false, std::memory_order_relaxed);
    osdDiagnosticEpoch_.fetch_add(1, std::memory_order_acq_rel);
    if (status != GVFG_OK)
    {
        if (error)
            *error = gvfgErrorMessage("gvfg_clear_channel_osd", status, handle_);
        return false;
    }
    osdSettings_.reset();
    osdSizeWarning_.clear();
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
    osdDiagnosticEnabled_.store(false, std::memory_order_relaxed);
    if (handle_)
    {
        gvfg_destroy(handle_);
        handle_ = nullptr;
    }
    openedDeviceIndex_ = -1;
    zeroCopyEnabled_ = false;
    cacheSignalStatus({});
}

bool GvfgSource::start(void *previewHwnd, int deviceIndex, int previewBitDepthMode,
                       bool zeroCopyEnabled, bool audioEnabled)
{
    stop();
    if (!open(deviceIndex, zeroCopyEnabled))
        return false;
    // open() may reuse an existing channel; do not validate pending OSD
    // against a signal size cached by an earlier session.
    if (!refreshSignalStatus())
    {
        emit errorOccurred(QStringLiteral("GVFG start blocked: cannot refresh the input signal."));
        return false;
    }
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

    if (osdSettings_)
    {
        OsdSettings settings = *osdSettings_;
        OsdPlacement placement;
        QString error;
        if (!normalizeOsdSettings(settings, &placement, &error))
        {
            emit errorOccurred(error);
            setPreview(nullptr, previewBitDepthMode);
            return false;
        }
        // A setting saved without a known frame size may turn out not to fit.
        // Keep it pending; the SDK skips OSD, not video capture, in that case.
        reportOsdSizeWarning(placement);
        st = applyOsdSettings(settings);
        if (st != GVFG_OK)
        {
            emit errorOccurred(gvfgErrorMessage("gvfg_set_channel_osd", st, handle_));
            setPreview(nullptr, previewBitDepthMode);
            return false;
        }
        osdSettings_ = std::move(settings);
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

void GvfgSource::cacheSignalStatus(const gvfg_signal_status_t &status)
{
    bool connectedChanged;
    bool geometryChanged;
    {
        std::lock_guard<std::mutex> lock(signalMutex_);
        connectedChanged = cachedSignal_.connected != status.connected;
        geometryChanged = connectedChanged || cachedSignal_.width != status.width ||
                          cachedSignal_.height != status.height;
        cachedSignal_ = status;
        if (geometryChanged)
            ++signalGeometryGeneration_;
    }
    if (connectedChanged)
        emit signalStatusChanged(status.connected != 0);
    if (geometryChanged)
        emit osdFrameSizeChanged();
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
            gvfg_signal_status_t status{};
            status.channel = GVFG_CHANNEL_0;
            cacheSignalStatus(status);
        }
        else if (eventType == GVFG_EVENT_VIDEO_FORMAT_CHANGED ||
                 eventType == GVFG_EVENT_VIDEO_INPUT_PLUGIN)
        {
            gvfg_signal_status_t status{};
            if (gvfg_get_channel_signal_status(handle_, GVFG_CHANNEL_0, &status) == GVFG_OK)
                cacheSignalStatus(status);
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

    cacheSignalStatus(status);
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
    uint32_t buildExperimentFlags = 0;
    if (handle_ && zeroCopyEnabled_ &&
        gvfg_debug_get_channel_osd_experiment_flags(handle_, GVFG_CHANNEL_0, 0,
                                                   &buildExperimentFlags) == GVFG_OK &&
        (buildExperimentFlags & GVFG_DEBUG_OSD_EXPERIMENT_ENABLED))
        emit eventOccurred(QStringLiteral("[OSD 實驗警告] 已啟用 driver zero-copy buffer 原地寫入。"
            "頁面可寫不代表 DMA 安全，可能畫面損壞或程式崩潰；僅供本機測試，不可出貨。"));
#ifdef QT6_VIEWER_ENABLE_DELIVERY_DIAGNOSTICS
    struct DeliveryBatch
    {
        uint64_t epoch = (std::numeric_limits<uint64_t>::max)();
        int width = 0;
        int height = 0;
        int format = 0;
        int stride = 0;
        int mode = 0; // 0=OFF, 1=applied, 2/3/4=requested but bypassed.
        uint32_t roiWidth = 0;
        uint32_t roiHeight = 0;
        uint32_t experimentFlags = 0;
        gvfg_debug_osd_page_check_t lastPageCheck{};
        bool lastPageCheckValid = false;
        unsigned int count = 0;
        unsigned int rebuilt = 0;
        double totalMs = 0;
        double maximumMs = 0;
        double acquisitionTotalMs = 0;
        double acquisitionMaximumMs = 0;
        double preparationMaximumMs = 0;
        double stageTotal[GVFG_DEBUG_OSD_STAGE_COUNT]{};
        double stageMaximum[GVFG_DEBUG_OSD_STAGE_COUNT]{};
    };
    DeliveryBatch deliveryBatch;
    static constexpr struct
    {
        gvfg_debug_osd_stage_t stage;
        const char *label;
    } displayedStages[] = {
        {GVFG_DEBUG_OSD_UPLOAD, "上傳原始畫面的疊字區域"},
        {GVFG_DEBUG_OSD_SUBMIT, "提交 GPU 疊字工作"},
        {GVFG_DEBUG_OSD_FRAME_COPY, "CPU 額外複製完整畫面"},
        {GVFG_DEBUG_OSD_READBACK_WAIT, "等待 GPU 完成並取得結果"},
        {GVFG_DEBUG_OSD_ROI_COPY, "CPU 將疊字結果貼回完整畫面"},
        {GVFG_DEBUG_OSD_BACKEND_RELEASE, "交付前釋放原始擷取畫面"}};
#endif
    while (!stopRequested_.load(std::memory_order_acquire))
    {
        uint64_t signalGeneration;
        {
            std::unique_lock<std::mutex> lock(signalMutex_);
            signalCv_.wait(lock, [this]() {
                return stopRequested_.load(std::memory_order_acquire) ||
                       signalConnected_.load(std::memory_order_acquire);
            });
            signalGeneration = signalGeometryGeneration_;
        }
        if (stopRequested_.load(std::memory_order_acquire))
            break;

        gvfg_frame_t frame{};
#ifdef QT6_VIEWER_ENABLE_DELIVERY_DIAGNOSTICS
        const uint64_t osdEpochBefore = osdDiagnosticEpoch_.load(std::memory_order_acquire);
        const bool osdRequested = osdDiagnosticEnabled_.load(std::memory_order_relaxed);
        const bool osdVisible = osdDiagnosticVisible_.load(std::memory_order_relaxed);
        const uint32_t osdTextWidth = osdDiagnosticTextWidth_.load(std::memory_order_relaxed);
        const uint32_t osdTextHeight = osdDiagnosticTextHeight_.load(std::memory_order_relaxed);
        gvfg_debug_frame_read_timing_t readTiming{};
        const auto &osdTiming = readTiming.osd;
        const gvfg_status_t st = handle_ ? gvfg_debug_read_channel_frame_profiled(
            handle_, GVFG_CHANNEL_0, &frame, &readTiming, 200) : GVFG_ESTATE;
#else
        const gvfg_status_t st = handle_ ? gvfg_read_channel_frame(handle_, GVFG_CHANNEL_0, &frame, 200) : GVFG_ESTATE;
#endif
#ifdef QT6_VIEWER_ENABLE_DELIVERY_DIAGNOSTICS
        // Take this timestamp before any preview, recording, mutex, or UI work.
        const uint64_t receivedNs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
        const uint64_t osdEpochAfter = osdDiagnosticEpoch_.load(std::memory_order_acquire);
        uint32_t experimentFlags = 0;
        const bool experimentFlagsValid = st == GVFG_OK &&
            gvfg_debug_get_channel_osd_experiment_flags(handle_, GVFG_CHANNEL_0,
                frame.frame_id, &experimentFlags) == GVFG_OK;
        gvfg_debug_osd_page_check_t pageCheck{};
        const bool pageCheckValid = experimentFlagsValid &&
            (experimentFlags & GVFG_DEBUG_OSD_EXPERIMENT_REJECTED) &&
            gvfg_debug_get_channel_osd_page_check(handle_, GVFG_CHANNEL_0,
                frame.frame_id, &pageCheck) == GVFG_OK;
#endif
        if (st == GVFG_OK)
        {
            bool connectedChanged = false;
            bool geometryChanged = false;
            {
                std::lock_guard<std::mutex> lock(signalMutex_);
                // A format/unplug event can overtake an in-flight read. Do
                // not restore old geometry (and re-clamp saved OSD) from it.
                if (signalGeneration == signalGeometryGeneration_)
                {
                    connectedChanged = cachedSignal_.connected == 0;
                    geometryChanged = connectedChanged || cachedSignal_.width != frame.width ||
                                      cachedSignal_.height != frame.height;
                    cachedSignal_.connected = 1;
                    cachedSignal_.channel = GVFG_CHANNEL_0;
                    cachedSignal_.width = frame.width;
                    cachedSignal_.height = frame.height;
                    cachedSignal_.pixel_format = frame.pixel_format;
                    cachedSignal_.bit_depth = frame.bit_depth;
                    if (geometryChanged)
                        ++signalGeometryGeneration_;
                }
            }
            if (connectedChanged)
                emit signalStatusChanged(true);
            if (geometryChanged)
                emit osdFrameSizeChanged();
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
#ifdef QT6_VIEWER_ENABLE_DELIVERY_DIAGNOSTICS
            // Report non-overlapping batches of exactly 300 valid frames.
            // Never mix settings, formats, or reads overlapping Apply/Clear.
            // Log after release via the existing queued signal, not disk I/O.
            if ((osdEpochBefore & 1u) != 0 || osdEpochBefore != osdEpochAfter ||
                frame.timestamp_ns == 0 || receivedNs < frame.timestamp_ns ||
                readTiming.acquisition_begin_ns == 0 || frame.timestamp_ns < readTiming.acquisition_begin_ns ||
                osdTiming.frame_id != frame.frame_id || !experimentFlagsValid)
                deliveryBatch = {};
            else
            {
                int mode = osdTiming.osd_applied ? 1 : 0;
                if (osdRequested && !osdTiming.osd_applied)
                    mode = osdTextWidth > static_cast<uint32_t>(frame.width) ||
                           osdTextHeight > static_cast<uint32_t>(frame.height) ? 2
                         : !osdVisible ? 3 : 4;
                if (deliveryBatch.epoch != osdEpochAfter || deliveryBatch.mode != mode ||
                    deliveryBatch.experimentFlags != experimentFlags ||
                    deliveryBatch.width != frame.width || deliveryBatch.height != frame.height ||
                    deliveryBatch.format != frame.pixel_format || deliveryBatch.stride != frame.row_stride_bytes ||
                    deliveryBatch.roiWidth != osdTiming.roi_width || deliveryBatch.roiHeight != osdTiming.roi_height)
                {
                    deliveryBatch = {};
                    deliveryBatch.epoch = osdEpochAfter;
                    deliveryBatch.mode = mode;
                    deliveryBatch.experimentFlags = experimentFlags;
                    deliveryBatch.width = frame.width;
                    deliveryBatch.height = frame.height;
                    deliveryBatch.format = frame.pixel_format;
                    deliveryBatch.stride = frame.row_stride_bytes;
                    deliveryBatch.roiWidth = osdTiming.roi_width;
                    deliveryBatch.roiHeight = osdTiming.roi_height;
                }
                const double acquisitionMs = static_cast<double>(frame.timestamp_ns - readTiming.acquisition_begin_ns) / 1e6;
                deliveryBatch.lastPageCheck = pageCheck;
                deliveryBatch.lastPageCheckValid = pageCheckValid;
                const double deliveryMs = static_cast<double>(receivedNs - readTiming.acquisition_begin_ns) / 1e6;
                deliveryBatch.totalMs += deliveryMs;
                deliveryBatch.maximumMs = (std::max)(deliveryBatch.maximumMs, deliveryMs);
                deliveryBatch.acquisitionTotalMs += acquisitionMs;
                deliveryBatch.acquisitionMaximumMs = (std::max)(deliveryBatch.acquisitionMaximumMs, acquisitionMs);
                deliveryBatch.rebuilt += osdTiming.surface_rebuilt ? 1u : 0u;
                // Compute the grouped maximum per frame, not by adding maxima
                // that may have occurred on different frames.
                const double preparationMs = osdTiming.stage_ms[GVFG_DEBUG_OSD_LOCK_WAIT] +
                    osdTiming.stage_ms[GVFG_DEBUG_OSD_SURFACE] + osdTiming.stage_ms[GVFG_DEBUG_OSD_PREPARE];
                deliveryBatch.preparationMaximumMs = (std::max)(deliveryBatch.preparationMaximumMs, preparationMs);
                for (int stage = 0; stage < GVFG_DEBUG_OSD_STAGE_COUNT; ++stage)
                {
                    deliveryBatch.stageTotal[stage] += osdTiming.stage_ms[stage];
                    deliveryBatch.stageMaximum[stage] = (std::max)(deliveryBatch.stageMaximum[stage], osdTiming.stage_ms[stage]);
                }
                if (++deliveryBatch.count == 300)
                {
                    const char *modeName = mode == 0 ? "OFF" : mode == 1 ? "ON"
                        : mode == 2 ? "ON，但文字太大，未疊字"
                        : mode == 3 ? "ON，但文字全為空白，未疊字" : "ON，但未疊字";
                    QString report = QStringLiteral("[OSD %1] 300 張耗時，單位 ms（平均 / 最大）")
                        .arg(QString::fromUtf8(modeName));
                    if (experimentFlags & GVFG_DEBUG_OSD_EXPERIMENT_USED)
                        report += QStringLiteral("\n  實驗模式：直接寫回 driver buffer，省略完整畫面複製；未保證 DMA 安全。");
                    else if (experimentFlags & GVFG_DEBUG_OSD_EXPERIMENT_REJECTED)
                    {
                        report += QStringLiteral("\n  實驗模式：記憶體頁面檢查未通過，已退回獨立輸出並複製完整畫面。");
                        if (deliveryBatch.lastPageCheckValid)
                        {
                            const auto &check = deliveryBatch.lastPageCheck;
                            const char *protection = "其他或未知屬性";
                            switch (check.protect & 0xffu)
                            {
                            case 0x01: protection = "PAGE_NOACCESS"; break;
                            case 0x02: protection = "PAGE_READONLY"; break;
                            case 0x04: protection = "PAGE_READWRITE"; break;
                            case 0x08: protection = "PAGE_WRITECOPY"; break;
                            case 0x10: protection = "PAGE_EXECUTE"; break;
                            case 0x20: protection = "PAGE_EXECUTE_READ"; break;
                            case 0x40: protection = "PAGE_EXECUTE_READWRITE"; break;
                            case 0x80: protection = "PAGE_EXECUTE_WRITECOPY"; break;
                            default: break;
                            }
                            const char *reason = "未知原因";
                            switch (check.reason)
                            {
                            case GVFG_DEBUG_OSD_PAGE_NO_POINTER: reason = "未取得可寫指標"; break;
                            case GVFG_DEBUG_OSD_PAGE_INVALID_RANGE: reason = "寫入範圍無效或位址溢位"; break;
                            case GVFG_DEBUG_OSD_PAGE_QUERY_FAILED: reason = "VirtualQuery 查詢失敗或回傳長度不符"; break;
                            case GVFG_DEBUG_OSD_PAGE_NOT_COMMITTED: reason = "頁面不是 MEM_COMMIT"; break;
                            case GVFG_DEBUG_OSD_PAGE_GUARD_OR_NOACCESS: reason = "頁面含 GUARD 或 NOACCESS"; break;
                            case GVFG_DEBUG_OSD_PAGE_NOT_READWRITE: reason = "頁面基本保護屬性不是 PAGE_READWRITE"; break;
                            case GVFG_DEBUG_OSD_PAGE_INVALID_REGION: reason = "查詢區域無效或位址溢位"; break;
                            default: break;
                            }
                            report += QStringLiteral("\n  拒絕原因（本組最後一張，其他張可能不同）：%1；reason=%2"
                                "，Protect=0x%3，State=0x%4，Type=0x%5，Win32Error=%6，QueryBytes=%7")
                                .arg(QString::fromUtf8(reason)).arg(check.reason)
                                .arg(check.protect, 0, 16).arg(check.state, 0, 16).arg(check.type, 0, 16)
                                .arg(check.win32_error).arg(check.query_bytes);
                            report += QStringLiteral("\n  Protect 基本屬性：%1；Win32Error 只在原生查詢失敗時有意義，"
                                "0 不代表頁面可寫。")
                                .arg(QString::fromUtf8(protection));
                        }
                        else
                            report += QStringLiteral("\n  拒絕細節查詢失敗，無法判斷頁面屬性。");
                    }
                    else if (zeroCopyEnabled_ && (experimentFlags & GVFG_DEBUG_OSD_EXPERIMENT_ENABLED))
                        report += QStringLiteral("\n  實驗開關已啟用，但這組 frame 未進行 driver buffer 原地疊字。");
                    const double preparationTotal = deliveryBatch.stageTotal[GVFG_DEBUG_OSD_LOCK_WAIT] +
                        deliveryBatch.stageTotal[GVFG_DEBUG_OSD_SURFACE] + deliveryBatch.stageTotal[GVFG_DEBUG_OSD_PREPARE];
                    report += QStringLiteral("\n  1. 取得原始畫面（含必要的複製）：%1 / %2")
                        .arg(deliveryBatch.acquisitionTotalMs / deliveryBatch.count, 0, 'f', 3)
                        .arg(deliveryBatch.acquisitionMaximumMs, 0, 'f', 3);
                    report += QStringLiteral("\n  2. 準備文字與輸出空間：%1 / %2")
                        .arg(preparationTotal / deliveryBatch.count, 0, 'f', 3)
                        .arg(deliveryBatch.preparationMaximumMs, 0, 'f', 3);
                    int step = 3;
                    for (const auto &item : displayedStages)
                    {
                        report += QStringLiteral("\n  %1. %2：%3 / %4").arg(step++)
                            .arg(QString::fromUtf8(item.label))
                            .arg(deliveryBatch.stageTotal[item.stage] / deliveryBatch.count, 0, 'f', 3)
                            .arg(deliveryBatch.stageMaximum[item.stage], 0, 'f', 3);
                    }
                    report += QStringLiteral("\n  取資料到交付上層總耗時（不含等下一張）：%1 / %2")
                        .arg(deliveryBatch.totalMs / deliveryBatch.count, 0, 'f', 3)
                        .arg(deliveryBatch.maximumMs, 0, 'f', 3);
                    if (deliveryBatch.rebuilt != 0)
                        report += QStringLiteral("\n  備註：這組包含 %1 次文字或 GPU 資源初始化。")
                            .arg(deliveryBatch.rebuilt);
                    emit eventOccurred(report);
                    deliveryBatch = {};
                }
            }
#endif
            {
                std::lock_guard<std::mutex> lock(audioPlaybackMutex_);
                if (audioPlayer_)
                    gvfg_audio_player_update_video_timestamp(audioPlayer_, frame.timestamp_ns);
            }
            continue;
        }

        if (st == GVFG_ETIMEOUT)
        {
#ifdef QT6_VIEWER_ENABLE_DELIVERY_DIAGNOSTICS
            deliveryBatch = {};
#endif
            continue;
        }
        if (stopRequested_.load(std::memory_order_acquire) || st == GVFG_ESTATE)
            break;

        emit errorOccurred(gvfgErrorMessage(
            "gvfg_read_channel_frame", st, handle_));
        break;
    }
}
