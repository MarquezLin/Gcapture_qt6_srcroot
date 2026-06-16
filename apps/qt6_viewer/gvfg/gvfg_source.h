#pragma once

#include <QObject>
#include <QImage>
#include <QString>
#include <QStringList>

#include <gvfg_capture.h>
#ifdef QT6_VIEWER_USE_STANDALONE_GVFG
#include <gvfg_preview.h>

#include <atomic>
#include <thread>
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
#else
    static void onFrame(const gvfg_frame_t *frame, void *user);
    static void onError(gvfg_status_t status, const char *message, void *user);
#endif

    gvfg_handle handle_ = nullptr;
#ifdef QT6_VIEWER_USE_STANDALONE_GVFG
    gvfg_preview_handle previewHandle_ = nullptr;
    std::thread readThread_;
    std::atomic_bool stopRequested_{false};
#endif
    bool running_ = false;
};
