#pragma once

#include <QObject>
#include <QImage>
#include <QString>
#include <QStringList>

#include <gvfg_capture.h>
#ifdef QT6_VIEWER_USE_STANDALONE_GVFG
#include <gvfg_preview.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#endif

#if defined(QT6_VIEWER_USE_STANDALONE_GVFG) && defined(QT6_VIEWER_ENABLE_GVFG_RECORDING)
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
#ifdef QT6_VIEWER_USE_STANDALONE_GVFG
    bool startRecording(const QString &path, int fpsNum, int fpsDen, int bitrateKbps, QString *error);
    void stopRecording();
    bool isRecording() const;
    uint64_t recordingFrames() const;
    QImage captureSnapshot(int timeoutMs, QString *error);
#endif

    gvfg_signal_status_t signalStatus() const;
    gvfg_runtime_info_t runtimeInfo() const;
    gvfg_preview_info_t previewInfo() const;

signals:
    void frameReady(const QImage &image);
    void errorOccurred(const QString &message);
    void preStartRuntimeInfoReady(const gvfg_runtime_info_t &info);

private:
#ifdef QT6_VIEWER_USE_STANDALONE_GVFG
    void readLoop();
    void writeRecordingFrame(const gvfg_frame_t &frame);
#else
    static void onFrame(const gvfg_frame_t *frame, void *user);
    static void onError(gvfg_status_t status, const char *message, void *user);
#endif

    gvfg_handle handle_ = nullptr;
#ifdef QT6_VIEWER_USE_STANDALONE_GVFG
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
    int recordingWidth_ = 0;
    int recordingHeight_ = 0;
    int recordingPixelFormat_ = GVFG_PIXFMT_UNKNOWN;
#ifdef QT6_VIEWER_ENABLE_GVFG_RECORDING
    std::unique_ptr<FfmpegVideoRecorder> recorder_;
#endif
#endif
    bool running_ = false;
};
