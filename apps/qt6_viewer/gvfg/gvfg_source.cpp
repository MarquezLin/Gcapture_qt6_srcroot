#include "gvfg_source.h"

#include <gvfg_debug.h>

#include <QAudioDevice>
#include <QAudioFormat>
#include <QAudioSink>
#include <QIODevice>
#include <QMediaDevices>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <limits>
#include <new>

#ifdef QT6_VIEWER_ENABLE_GVFG_RECORDING
#include "ffmpeg_video_recorder.h"
#endif

namespace
{
constexpr size_t kMaxQueuedPlaybackAudioPackets = 10;

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
        emit errorOccurred(QStringLiteral("gvfg_set_channel_audio_enabled failed: %1")
                               .arg(QString::fromUtf8(gvfg_strerror(st))));
        setPreview(nullptr, previewBitDepthMode);
        return false;
    }

    audioFormat_ = {};
    if (audioEnabled)
    {
        st = gvfg_get_channel_audio_format(handle_, GVFG_CHANNEL_0, &audioFormat_);
        if (st != GVFG_OK)
        {
            emit errorOccurred(QStringLiteral("gvfg_get_channel_audio_format failed: %1")
                                   .arg(QString::fromUtf8(gvfg_strerror(st))));
            setPreview(nullptr, previewBitDepthMode);
            return false;
        }
        if (audioFormat_.sample_rate == 0 || audioFormat_.channels == 0 ||
            (audioFormat_.bits_per_sample != 8 &&
             audioFormat_.bits_per_sample != 16 &&
             audioFormat_.bits_per_sample != 32))
        {
            emit errorOccurred(QStringLiteral("GVFG audio format is not supported by Qt playback: %1 Hz, %2 ch, %3-bit")
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
        emit errorOccurred(QStringLiteral("gvfg_start failed: %1").arg(QString::fromUtf8(gvfg_strerror(st))));
        setPreview(nullptr, previewBitDepthMode);
        return false;
    }

    running_ = true;
    audioEnabled_ = audioEnabled;
    stopRequested_.store(false, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(audioPlaybackMutex_);
        audioPlaybackQueue_.clear();
    }
    readThread_ = std::thread([this]() { readLoop(); });
    if (audioEnabled_)
    {
        audioPlaybackThread_ = std::thread([this]() { audioPlaybackLoop(); });
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
    audioVolume_.store(std::clamp(volume, 0.0f, 1.0f), std::memory_order_release);
}

void GvfgSource::stop()
{
    running_ = false;
    stopRequested_.store(true, std::memory_order_release);
    snapshotCv_.notify_all();
    audioPlaybackCv_.notify_all();
    if (readThread_.joinable())
        readThread_.join();
    if (audioThread_.joinable())
        audioThread_.join();
    audioPlaybackCv_.notify_all();
    if (audioPlaybackThread_.joinable())
        audioPlaybackThread_.join();
    {
        std::lock_guard<std::mutex> lock(audioPlaybackMutex_);
        audioPlaybackQueue_.clear();
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
        gvfg_audio_frame_t frame{};
        const gvfg_status_t st = gvfg_read_channel_audio_frame(
            handle_, GVFG_CHANNEL_0, &frame, 200);
        if (st == GVFG_ETIMEOUT)
            continue;
        if (st != GVFG_OK)
        {
            if (!stopRequested_.load(std::memory_order_acquire))
                emit errorOccurred(QStringLiteral("gvfg_read_channel_audio_frame failed: %1")
                                       .arg(QString::fromUtf8(gvfg_strerror(st))));
            break;
        }

        const auto *begin = static_cast<const uint8_t *>(frame.data);
        std::vector<uint8_t> pcm(begin, begin + frame.data_size);

        const gvfg_status_t releaseStatus =
            gvfg_release_channel_audio_frame(handle_, GVFG_CHANNEL_0, &frame);
        if (releaseStatus != GVFG_OK)
        {
            if (!stopRequested_.load(std::memory_order_acquire))
                emit errorOccurred(QStringLiteral("gvfg_release_channel_audio_frame failed: %1")
                                       .arg(QString::fromUtf8(gvfg_strerror(releaseStatus))));
            break;
        }

#ifdef QT6_VIEWER_ENABLE_GVFG_RECORDING
        {
            std::lock_guard<std::mutex> lock(recordingMutex_);
            if (recording_ && !recordingStopRequested_)
            {
                QueuedRecordingAudio queued;
                queued.data = pcm;
                constexpr size_t kMaxQueuedAudioPackets = 64;
                if (recordingAudioQueue_.size() >= kMaxQueuedAudioPackets)
                    recordingAudioQueue_.pop_front();
                recordingAudioQueue_.push_back(std::move(queued));
                recordingCv_.notify_one();
            }
        }
#endif

        {
            std::lock_guard<std::mutex> lock(audioPlaybackMutex_);
            if (audioPlaybackQueue_.size() >= kMaxQueuedPlaybackAudioPackets)
                audioPlaybackQueue_.pop_front();
            audioPlaybackQueue_.push_back(std::move(pcm));
        }
        audioPlaybackCv_.notify_one();
    }
    audioPlaybackCv_.notify_all();
}

void GvfgSource::audioPlaybackLoop()
{
    const gvfg_audio_format_t format = audioFormat_;
    QAudioFormat outputFormat;
    outputFormat.setSampleRate(static_cast<int>(format.sample_rate));
    outputFormat.setChannelCount(static_cast<int>(format.channels));
    switch (format.bits_per_sample)
    {
    case 8:
        outputFormat.setSampleFormat(QAudioFormat::UInt8);
        break;
    case 16:
        outputFormat.setSampleFormat(QAudioFormat::Int16);
        break;
    case 32:
        outputFormat.setSampleFormat(QAudioFormat::Int32);
        break;
    default:
        return;
    }

    const QAudioDevice outputDevice = QMediaDevices::defaultAudioOutput();
    if (outputDevice.isNull() || !outputDevice.isFormatSupported(outputFormat))
    {
        emit errorOccurred(QStringLiteral("Default Qt audio output does not support GVFG PCM: %1 Hz, %2 ch, %3-bit")
                               .arg(format.sample_rate)
                               .arg(format.channels)
                               .arg(format.bits_per_sample));
        return;
    }

    QAudioSink audioSink(outputDevice, outputFormat);
    float appliedVolume = audioVolume_.load(std::memory_order_acquire);
    audioSink.setVolume(appliedVolume);
    QIODevice *audioOutput = audioSink.start();
    if (!audioOutput)
    {
        emit errorOccurred(QStringLiteral("Qt audio output failed to start for GVFG PCM."));
        return;
    }

    emit eventOccurred(QStringLiteral("Audio playback started via Qt: %1 Hz, %2 ch, %3-bit PCM")
                           .arg(format.sample_rate)
                           .arg(format.channels)
                           .arg(format.bits_per_sample));

    while (!stopRequested_.load(std::memory_order_acquire))
    {
        const float requestedVolume = audioVolume_.load(std::memory_order_acquire);
        if (requestedVolume != appliedVolume)
        {
            audioSink.setVolume(requestedVolume);
            appliedVolume = requestedVolume;
        }

        std::vector<uint8_t> pcm;
        {
            std::unique_lock<std::mutex> lock(audioPlaybackMutex_);
            audioPlaybackCv_.wait(lock, [this]() {
                return stopRequested_.load(std::memory_order_acquire) ||
                       !audioPlaybackQueue_.empty();
            });
            if (stopRequested_.load(std::memory_order_acquire))
                break;
            pcm = std::move(audioPlaybackQueue_.front());
            audioPlaybackQueue_.pop_front();
        }

        qint64 written = 0;
        while (written < static_cast<qint64>(pcm.size()) &&
               !stopRequested_.load(std::memory_order_acquire))
        {
            const qint64 result = audioOutput->write(
                reinterpret_cast<const char *>(pcm.data()) + written,
                static_cast<qint64>(pcm.size()) - written);
            if (result < 0)
            {
                emit errorOccurred(QStringLiteral("Qt audio output write failed for GVFG PCM."));
                audioSink.stop();
                return;
            }
            if (result == 0)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            written += result;
        }
    }
    audioSink.stop();
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
        if (recording_ || recordingOpenPending_)
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
        std::unique_lock<std::mutex> lock(recordingMutex_);
        recordingCv_.wait(lock, [this]() { return !recordingCopyInProgress_; });
        if (recording_ || recordingOpenPending_)
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

    const size_t sourceBytesPerPixel = pixelFormat == GVFG_PIXFMT_Y210 ? 4u : 2u;
    const size_t sourceWidth = static_cast<size_t>(width);
    const size_t sourceHeight = static_cast<size_t>(height);
    if (sourceWidth > (std::numeric_limits<size_t>::max)() / sourceBytesPerPixel)
    {
        if (error)
            *error = QStringLiteral("GVFG recording RAW row size is too large.");
        return false;
    }
    const size_t sourceRowBytes = sourceWidth * sourceBytesPerPixel;
    if (sourceHeight > 0 && sourceRowBytes > (std::numeric_limits<size_t>::max)() / sourceHeight)
    {
        if (error)
            *error = QStringLiteral("GVFG recording RAW buffer size is too large.");
        return false;
    }
    const size_t rawBufferBytes = sourceRowBytes * sourceHeight;
    const size_t nv12Rows = sourceHeight + sourceHeight / 2u;
    if (nv12Rows > 0 && sourceWidth > (std::numeric_limits<size_t>::max)() / nv12Rows)
    {
        if (error)
            *error = QStringLiteral("GVFG recording NV12 buffer size is too large.");
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(recordingMutex_);
        if (recording_ || recordingOpenPending_)
        {
            if (error)
                *error = QStringLiteral("GVFG recording is already running.");
            return false;
        }
        try
        {
            recordingSlots_.resize(kRecordingSlotCount);
            for (RecordingSlot &slot : recordingSlots_)
                slot.rawData.resize(rawBufferBytes);
            recordingNv12Buffer_.resize(sourceWidth * nv12Rows);
        }
        catch (const std::bad_alloc &)
        {
            if (error)
                *error = QStringLiteral("Unable to allocate the GVFG recording buffer pool.");
            return false;
        }
        recordingConfig_.path = path.toStdString();
        recordingConfig_.fpsNum = fpsNum > 0 ? fpsNum : 30;
        recordingConfig_.fpsDen = fpsDen > 0 ? fpsDen : 1;
        recordingConfig_.bitrateKbps = bitrateKbps > 0 ? bitrateKbps : 8000;
        recording_ = false;
        recordingOpenPending_ = true;
        recordingStopRequested_ = false;
        recordingStartError_.clear();
        recordingFrames_ = 0;
        recordingDroppedFrames_ = 0;
        recordingFirstFrameId_ = 0;
        recordingWidth_ = width;
        recordingHeight_ = height;
        recordingPixelFormat_ = pixelFormat;
        freeRecordingSlots_.clear();
        pendingRecordingSlots_.clear();
        freeRecordingSlots_.reserve(kRecordingSlotCount);
        pendingRecordingSlots_.reserve(kRecordingSlotCount);
        for (size_t index = 0; index < recordingSlots_.size(); ++index)
            freeRecordingSlots_.push_back(index);
        recordingAudioQueue_.clear();
    }
    try
    {
        recordingThread_ = std::thread([this]() { recordingLoop(); });
    }
    catch (...)
    {
        std::lock_guard<std::mutex> lock(recordingMutex_);
        recordingOpenPending_ = false;
        recordingStopRequested_ = true;
        recordingStartError_ = QStringLiteral("Unable to start the GVFG recording worker thread.");
        if (error)
            *error = recordingStartError_;
        return false;
    }

    bool started = false;
    QString startError;
    {
        std::unique_lock<std::mutex> lock(recordingMutex_);
        recordingCv_.wait(lock, [this]() { return !recordingOpenPending_; });
        started = recording_;
        startError = recordingStartError_;
    }
    if (!started)
    {
        if (recordingThread_.joinable())
            recordingThread_.join();
        if (error)
            *error = startError.isEmpty()
                         ? QStringLiteral("GVFG recording encoder failed to start.")
                         : startError;
        return false;
    }
    return true;
#endif
}

void GvfgSource::stopRecording()
{
    {
        std::unique_lock<std::mutex> lock(recordingMutex_);
        recording_ = false;
#ifdef QT6_VIEWER_ENABLE_GVFG_RECORDING
        recordingStopRequested_ = true;
        recordingCv_.notify_all();
        recordingCv_.wait(lock, [this]() { return !recordingCopyInProgress_; });
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
        pendingRecordingSlots_.clear();
        freeRecordingSlots_.clear();
        for (size_t index = 0; index < recordingSlots_.size(); ++index)
            freeRecordingSlots_.push_back(index);
        recordingAudioQueue_.clear();
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

uint64_t GvfgSource::recordingDroppedFrames() const
{
    std::lock_guard<std::mutex> lock(recordingMutex_);
    return recordingDroppedFrames_;
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

    const int sourceRowBytes = frame.width *
        ((frame.pixel_format == GVFG_PIXFMT_Y210) ? 4 : 2);
    if (!frameHasRows(frame, sourceRowBytes) ||
        (frame.width & 1) != 0 || (frame.height & 1) != 0)
    {
        {
            std::lock_guard<std::mutex> lock(recordingMutex_);
            recording_ = false;
            recordingStopRequested_ = true;
            pendingRecordingSlots_.clear();
            recordingAudioQueue_.clear();
        }
        recordingCv_.notify_all();
        emit errorOccurred(QStringLiteral("GVFG recording stopped: invalid frame layout"));
        return;
    }

    bool formatChanged = false;
    size_t slotIndex = 0;
    RecordingSlot *slot = nullptr;
    {
        std::lock_guard<std::mutex> lock(recordingMutex_);
        if (!recording_ || recordingStopRequested_)
            return;
        if (frame.width != recordingWidth_ || frame.height != recordingHeight_ ||
            frame.pixel_format != recordingPixelFormat_)
        {
            recording_ = false;
            recordingStopRequested_ = true;
            pendingRecordingSlots_.clear();
            recordingCv_.notify_all();
            formatChanged = true;
        }
        else if (freeRecordingSlots_.empty())
        {
            ++recordingDroppedFrames_;
        }
        else
        {
            slotIndex = freeRecordingSlots_.back();
            freeRecordingSlots_.pop_back();
            slot = &recordingSlots_[slotIndex];
            slot->width = frame.width;
            slot->height = frame.height;
            slot->rowStrideBytes = sourceRowBytes;
            slot->pixelFormat = frame.pixel_format;
            slot->bitDepth = frame.bit_depth;
            slot->frameId = frame.frame_id;

            if (recordingFirstFrameId_ == 0)
                recordingFirstFrameId_ = frame.frame_id;
            slot->pts = static_cast<int64_t>(frame.frame_id - recordingFirstFrameId_);
            recordingCopyInProgress_ = true;
        }
    }
    if (formatChanged)
    {
        emit errorOccurred(QStringLiteral("GVFG recording stopped: frame format changed while recording"));
        return;
    }
    if (!slot)
        return;

    const auto *source = static_cast<const uint8_t *>(frame.data);
    for (int row = 0; row < frame.height; ++row)
    {
        std::memcpy(slot->rawData.data() + static_cast<size_t>(row) *
                        static_cast<size_t>(sourceRowBytes),
                    source + static_cast<size_t>(row) *
                        static_cast<size_t>(frame.row_stride_bytes),
                    static_cast<size_t>(sourceRowBytes));
    }

    {
        std::lock_guard<std::mutex> lock(recordingMutex_);
        recordingCopyInProgress_ = false;
        if (recording_ && !recordingStopRequested_)
            pendingRecordingSlots_.push_back(slotIndex);
        else
            freeRecordingSlots_.push_back(slotIndex);
    }
    recordingCv_.notify_all();
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
    cfg.audio_enabled = audioEnabled_;
    cfg.audio_sample_rate = static_cast<int>(audioFormat_.sample_rate);
    cfg.audio_channels = static_cast<int>(audioFormat_.channels);
    cfg.audio_bits_per_sample = static_cast<int>(audioFormat_.bits_per_sample);

    FfmpegVideoRecorder recorder;
    std::string error;
    if (!recorder.open(cfg, &error))
    {
        {
            std::lock_guard<std::mutex> lock(recordingMutex_);
            recording_ = false;
            recordingOpenPending_ = false;
            recordingStopRequested_ = true;
            recordingStartError_ = QString::fromStdString(error);
            pendingRecordingSlots_.clear();
            recordingAudioQueue_.clear();
        }
        recordingCv_.notify_all();
        return;
    }
    bool startCancelled = false;
    {
        std::lock_guard<std::mutex> lock(recordingMutex_);
        startCancelled = recordingStopRequested_;
        recording_ = !startCancelled;
        recordingOpenPending_ = false;
        recordingStartError_ = startCancelled
                                   ? QStringLiteral("GVFG recording start was cancelled.")
                                   : QString();
    }
    recordingCv_.notify_all();
    if (startCancelled)
    {
        recorder.close();
        return;
    }

    while (true)
    {
        size_t recordingSlotIndex = 0;
        QueuedRecordingAudio queuedAudio;
        bool haveVideo = false;
        bool haveAudio = false;
        {
            std::unique_lock<std::mutex> lock(recordingMutex_);
            recordingCv_.wait(lock, [this]() {
                return recordingStopRequested_ || !pendingRecordingSlots_.empty() || !recordingAudioQueue_.empty();
            });
            if (!recordingAudioQueue_.empty())
            {
                queuedAudio = std::move(recordingAudioQueue_.front());
                recordingAudioQueue_.pop_front();
                haveAudio = true;
            }
            if (!pendingRecordingSlots_.empty())
            {
                recordingSlotIndex = pendingRecordingSlots_.front();
                pendingRecordingSlots_.erase(pendingRecordingSlots_.begin());
                haveVideo = true;
            }
            if (!haveAudio && !haveVideo)
            {
                if (recordingStopRequested_)
                    break;
                continue;
            }
        }

        if (haveAudio)
        {
            if (!recorder.writeAudio(queuedAudio.data.data(), queuedAudio.data.size(), &error))
            {
                {
                    std::lock_guard<std::mutex> lock(recordingMutex_);
                    recording_ = false;
                    recordingStopRequested_ = true;
                    pendingRecordingSlots_.clear();
                    recordingAudioQueue_.clear();
                }
                emit errorOccurred(QStringLiteral("GVFG recording stopped: %1").arg(QString::fromStdString(error)));
                break;
            }
        }

        if (!haveVideo)
            continue;

        if (recordingSlotIndex >= recordingSlots_.size())
        {
            {
                std::lock_guard<std::mutex> lock(recordingMutex_);
                recording_ = false;
                recordingStopRequested_ = true;
                pendingRecordingSlots_.clear();
                recordingAudioQueue_.clear();
            }
            emit errorOccurred(QStringLiteral("GVFG recording stopped: invalid recording slot index"));
            break;
        }

        RecordingSlot &queued = recordingSlots_[recordingSlotIndex];
        gvfg_frame_t sourceFrame{};
        sourceFrame.data = queued.rawData.data();
        sourceFrame.data_size = static_cast<uint64_t>(queued.rawData.size());
        sourceFrame.width = queued.width;
        sourceFrame.height = queued.height;
        sourceFrame.row_stride_bytes = queued.rowStrideBytes;
        sourceFrame.pixel_format = queued.pixelFormat;
        sourceFrame.bit_depth = queued.bitDepth;
        sourceFrame.frame_id = queued.frameId;
        const gvfg_status_t convertStatus = gvfg_gpu_convert_to_nv12(
            &sourceFrame,
            recordingNv12Buffer_.data(),
            static_cast<uint64_t>(recordingNv12Buffer_.size()),
            queued.width);
        if (convertStatus != GVFG_OK)
        {
            {
                std::lock_guard<std::mutex> lock(recordingMutex_);
                recording_ = false;
                recordingStopRequested_ = true;
                pendingRecordingSlots_.clear();
                recordingAudioQueue_.clear();
            }
            emit errorOccurred(QStringLiteral("GVFG recording stopped: NV12 conversion failed: %1")
                                   .arg(QString::fromUtf8(gvfg_strerror(convertStatus))));
            break;
        }

        FfmpegVideoFrameView view{};
        view.format = GCAP_FMT_NV12;
        view.width = queued.width;
        view.height = queued.height;
        view.data[0] = recordingNv12Buffer_.data();
        view.data[1] = recordingNv12Buffer_.data() +
                       static_cast<size_t>(queued.width) * static_cast<size_t>(queued.height);
        view.stride[0] = queued.width;
        view.stride[1] = queued.width;
        view.pts = queued.pts;
        if (!recorder.writeFrame(view, &error))
        {
            {
                std::lock_guard<std::mutex> lock(recordingMutex_);
                recording_ = false;
                recordingStopRequested_ = true;
                pendingRecordingSlots_.clear();
                recordingAudioQueue_.clear();
            }
            emit errorOccurred(QStringLiteral("GVFG recording stopped: %1").arg(QString::fromStdString(error)));
            break;
        }
        {
            std::lock_guard<std::mutex> lock(recordingMutex_);
            freeRecordingSlots_.push_back(recordingSlotIndex);
            ++recordingFrames_;
        }
    }
    recorder.close();
#endif
}
