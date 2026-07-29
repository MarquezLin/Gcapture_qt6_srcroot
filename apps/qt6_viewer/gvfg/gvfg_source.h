#pragma once

#include <QObject>
#include <QImage>
#include <QString>
#include <QStringList>

#include <gvfg_capture.h>
#include <gvfg_preview.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef QT6_VIEWER_ENABLE_GVFG_RECORDING
class FfmpegVideoRecorder;
#endif

class GvfgSource : public QObject
{
    Q_OBJECT

public:
    explicit GvfgSource(QObject *parent = nullptr);
    ~GvfgSource() override;

    static QStringList enumerateDevices();

    bool start(void *previewHwnd, int deviceIndex, int previewBitDepthMode);
    bool setPreview(void *previewHwnd, int previewBitDepthMode);
    void stop();
    bool isRunning() const { return running_; }
    bool startRecording(const QString &path, int fpsNum, int fpsDen, int bitrateKbps, bool useHevc, QString *error);
    void stopRecording();
    bool isRecording() const;
    uint64_t recordingFrames() const;
    QImage captureSnapshot(int timeoutMs, QString *error);

    gvfg_signal_status_t signalStatus() const;
    gvfg_runtime_info_t runtimeInfo() const;
    gvfg_preview_info_t previewInfo() const;
    gvfg_preview_stats_t previewStats() const;

signals:
    void frameReady(const QImage &image);
    void errorOccurred(const QString &message);
    void preStartRuntimeInfoReady(const gvfg_runtime_info_t &info);

private:
    void readLoop();
    void writeRecordingFrame(const gvfg_frame_t &frame);
#ifdef QT6_VIEWER_ENABLE_GVFG_RECORDING
    void recordingLoop();
#endif

    gvfg_handle handle_ = nullptr;
    gvfg_preview_handle previewHandle_ = nullptr;
    std::thread readThread_;
    std::atomic_bool stopRequested_{false};
    mutable std::mutex recordingMutex_;
    std::mutex snapshotMutex_;
    std::condition_variable snapshotCv_;
    bool snapshotPending_ = false;
    bool snapshotComplete_ = false;
    QImage snapshotImage_;
    QString snapshotError_;
    bool recording_ = false;
    uint64_t recordingFrames_ = 0;
    uint64_t recordingDroppedFrames_ = 0;
    int recordingWidth_ = 0;
    int recordingHeight_ = 0;
    int recordingPixelFormat_ = GVFG_PIXFMT_UNKNOWN;
#ifdef QT6_VIEWER_ENABLE_GVFG_RECORDING
    struct RecordingConfig
    {
        std::string path;
        int fpsNum = 30;
        int fpsDen = 1;
        int bitrateKbps = 8000;
        bool useHevc = false;
    };

    bool recordingOpenPending_ = false;
    bool recordingStopRequested_ = false;
    uint64_t recordingFirstFrameId_ = 0;
    RecordingConfig recordingConfig_;
    struct QueuedRecordingFrame
    {
        std::vector<uint8_t> data;
        int width = 0;
        int height = 0;
        int pixelFormat = GVFG_PIXFMT_UNKNOWN;
        int stride = 0;
        int64_t pts = 0;
    };
    std::condition_variable recordingCv_;
    std::deque<QueuedRecordingFrame> recordingQueue_;
    std::thread recordingThread_;
#endif
    bool running_ = false;
};
