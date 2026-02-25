#pragma once

#include <QObject>
#include <QImage>
#include <QMutex>
#include <QString>
#include <cstdint>
#include <atomic>
#include <thread>

// Linked CaptureSDK API
#include "capture_sdk.h"
#include "capture_sdk_log.h"

// A Qt-friendly wrapper for CaptureSDK.
//
// - Linked against CaptureSDK.lib (VS2022 x64).
// - Requires CaptureSDK.dll at runtime (copied next to qt6_viewer.exe).
// - In CAP_MODE_CONTINUOUS, this SDK is threadless: the app pumps frames by
//   calling cap_capture_step(). The callback is invoked in the caller thread of
//   cap_capture_step().
// - Frames are copied into QImage and delivered via the frameReady() signal.
class CaptureSdkSource : public QObject
{
    Q_OBJECT

public:
    explicit CaptureSdkSource(QObject *parent = nullptr);
    ~CaptureSdkSource() override;

    bool isLoaded() const;
    QString lastError() const;

    // Start capturing.
    // width/height: pass 0,0 for "SDK default"; wrapper will fallback if needed.
    bool start(int width = 0, int height = 0);
    void stop();

signals:
    void frameReady(const QImage &img);
    void errorOccurred(const QString &msg);

private:
    void setError(const QString &msg);

    static void __stdcall s_video_cb(const uint8_t *buf,
                                     int width,
                                     int height,
                                     int bytes_per_pixel,
                                     void *user);
    void onVideo(const uint8_t *buf, int width, int height, int bpp);

    cap_handle_t handle_ = nullptr;
    bool capturing_ = false;

    // Optional timeout-capable step API (resolved from the DLL at runtime).
    using cap_capture_step_timeout_fn = cap_result_t (*)(cap_handle_t, int);
    cap_capture_step_timeout_fn stepTimeout_ = nullptr;

    std::atomic<bool> running_{false};
    std::thread pumpThread_;

    mutable QMutex mtx_;
    QString lastError_;
};
