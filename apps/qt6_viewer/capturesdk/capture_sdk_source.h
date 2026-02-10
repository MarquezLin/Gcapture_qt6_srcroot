#pragma once

#include <QObject>
#include <QImage>
#include <QMutex>
#include <QString>
#include <cstdint>

#ifdef _WIN32
  #include <windows.h>
#endif

// A Qt-friendly wrapper for CaptureSDK.dll.
//
// - No import library required: LoadLibrary + GetProcAddress.
// - SDK delivers frames via callback on an internal thread.
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
    // CaptureSDK API (we treat cap_result_t as int)
    using cap_handle_t = void *;
    using cap_result_t = int;
    using cap_video_cb_t = void (*)(const uint8_t *buf,
                                   int width,
                                   int height,
                                   int bytes_per_pixel,
                                   void *user);

    using cap_create_t        = cap_result_t (*)(cap_handle_t *out);
    using cap_destroy_t       = cap_result_t (*)(cap_handle_t h);
    using cap_init_t          = cap_result_t (*)(cap_handle_t h, int width, int height);
    using cap_uninit_t        = cap_result_t (*)(cap_handle_t h);
    using cap_start_capture_t = cap_result_t (*)(cap_handle_t h, cap_video_cb_t cb, void *user);
    using cap_stop_capture_t  = cap_result_t (*)(cap_handle_t h);

    bool loadDll();
    void unloadDll();
    void setError(const QString &msg);

    static void s_video_cb(const uint8_t *buf,
                           int width,
                           int height,
                           int bytes_per_pixel,
                           void *user);
    void onVideo(const uint8_t *buf, int width, int height, int bpp);

#ifdef _WIN32
    HMODULE dll_ = nullptr;
#endif

    // Function pointers
    cap_create_t        cap_create_ = nullptr;
    cap_destroy_t       cap_destroy_ = nullptr;
    cap_init_t          cap_init_ = nullptr;
    cap_uninit_t        cap_uninit_ = nullptr;
    cap_start_capture_t cap_start_capture_ = nullptr;
    cap_stop_capture_t  cap_stop_capture_ = nullptr;

    cap_handle_t handle_ = nullptr;
    bool capturing_ = false;

    mutable QMutex mtx_;
    QString lastError_;
};
