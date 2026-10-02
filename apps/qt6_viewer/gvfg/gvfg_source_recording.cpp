#include "gvfg_source.h"

#include "ffmpeg_video_recorder.h"

#include <cstring>
#include <limits>
#include <new>

namespace
{
bool recordingFrameHasRows(const gvfg_frame_t &frame, int minRowBytes)
{
    if (!frame.data || frame.width <= 0 || frame.height <= 0 ||
        frame.row_stride_bytes < minRowBytes || minRowBytes <= 0)
        return false;
    const uint64_t required = static_cast<uint64_t>(frame.row_stride_bytes) *
                                  static_cast<uint64_t>(frame.height - 1) +
                              static_cast<uint64_t>(minRowBytes);
    return frame.data_size >= required;
}
} // namespace

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
        recordingTimelineOriginNs_ = latestVideoTimestampNs_.load(std::memory_order_acquire);
        recordingLastVideoPts_ = -1;
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
    if (!recordingFrameHasRows(frame, sourceRowBytes) ||
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

            const uint64_t elapsedNs = frame.timestamp_ns > recordingTimelineOriginNs_
                                           ? frame.timestamp_ns - recordingTimelineOriginNs_
                                           : 0;
            const uint64_t ptsDenominator = 1'000'000'000ULL *
                                            static_cast<uint64_t>(recordingConfig_.fpsDen);
            int64_t timestampPts = static_cast<int64_t>(
                (elapsedNs * static_cast<uint64_t>(recordingConfig_.fpsNum) +
                 ptsDenominator / 2) / ptsDenominator);
            if (timestampPts <= recordingLastVideoPts_)
                timestampPts = recordingLastVideoPts_ + 1;
            slot->pts = timestampPts;
            recordingLastVideoPts_ = timestampPts;
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
            const int64_t audioTimelineNs = queuedAudio.timestampNs > recordingTimelineOriginNs_
                                                ? static_cast<int64_t>(queuedAudio.timestampNs -
                                                                       recordingTimelineOriginNs_)
                                                : 0;
            if (!recorder.writeAudio(queuedAudio.data.data(), queuedAudio.data.size(),
                                     audioTimelineNs, &error))
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
