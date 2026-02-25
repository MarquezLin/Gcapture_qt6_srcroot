#include "capture_sdk_source.h"
#include <QDebug>
#include <QThread>
#include <chrono>
#ifdef _WIN32
#include <windows.h>
#include <objbase.h> // CoInitializeEx, CoUninitialize, COINIT_*
#endif

// Uses linked CaptureSDK API (capture_sdk.h)
// 全域指標，讓 log callback 能找到 Qt 物件
static CaptureSdkSource *g_log_receiver = nullptr;

static void s_log_callback(cap_log_level_t level, const char *msg)
{
    if (!g_log_receiver || !msg)
        return;

    QString qmsg = QString::fromUtf8(msg);

    // 一定要切回 UI thread
    QMetaObject::invokeMethod(
        g_log_receiver,
        [qmsg, level]()
        {
            qDebug().noquote() << qmsg;
        },
        Qt::QueuedConnection);
}

CaptureSdkSource::CaptureSdkSource(QObject *parent)
    : QObject(parent)
{
}

CaptureSdkSource::~CaptureSdkSource()
{
    stop();
}

bool CaptureSdkSource::isLoaded() const
{
    // Linked against CaptureSDK.lib; if the app starts, the symbols are resolved.
    return true;
}

QString CaptureSdkSource::lastError() const
{
    QMutexLocker lk(&mtx_);
    return lastError_;
}

void CaptureSdkSource::setError(const QString &msg)
{
    {
        QMutexLocker lk(&mtx_);
        lastError_ = msg;
    }
    emit errorOccurred(msg);
}

bool CaptureSdkSource::start(int width, int height)
{
    if (capturing_)
        return true;

    if (!handle_)
    {
        cap_handle_t h = nullptr;
        const cap_result_t rc = cap_create(&h);
        if (rc != CAP_OK || !h)
        {
            setError(QStringLiteral("cap_create failed (%1)").arg((int)rc));
            return false;
        }
        handle_ = h;
    }

    g_log_receiver = this;
    cap_set_log_callback(&s_log_callback);

    // Try (0,0) as "SDK default" first if caller didn't specify.
    int w = width;
    int h = height;
    if (w < 0)
        w = 0;
    if (h < 0)
        h = 0;

    cap_result_t rc = cap_init(handle_, w, h);
    qDebug() << "[cap] init ret=" << rc;
    if (rc != CAP_OK)
    {
        cap_set_log_callback(nullptr);
        g_log_receiver = nullptr;
        setError(QStringLiteral("cap_init failed (%1)").arg((int)rc));
        return false;
    }

    // Use continuous mode for live preview.
    rc = cap_start_capture(handle_, CAP_MODE_CONTINUOUS, &CaptureSdkSource::s_video_cb, this);
    qDebug() << "[cap] start ret=" << rc;
    if (rc != CAP_OK)
    {
        cap_set_log_callback(nullptr);
        g_log_receiver = nullptr;
        setError(QStringLiteral("cap_start_capture failed (%1)").arg((int)rc));
        cap_uninit(handle_);
        return false;
    }

    capturing_ = true;
    // IMPORTANT: enable pumping BEFORE starting the pump thread.
    // If running_ stays false, the pump thread exits immediately and you'll see no frames.
    running_ = true;

#ifdef _WIN32
    // Resolve optional timeout API from the loaded DLL.
    if (!stepTimeout_)
    {
        HMODULE hMod = GetModuleHandleW(L"CaptureSDK.dll");
        if (hMod)
        {
            stepTimeout_ = reinterpret_cast<cap_capture_step_timeout_fn>(
                GetProcAddress(hMod, "cap_capture_step_timeout"));
        }
    }
#endif

    // Threadless continuous mode: pump frames from an application-owned thread.
    pumpThread_ = std::thread([this]
                              {
    while (running_)
    {
        cap_result_t stepRc = CAP_E_INTERNAL;

        if (stepTimeout_)
        {
            stepRc = stepTimeout_(handle_, 50);
            if (stepRc == CAP_E_TIMEOUT)
                continue;
        }
        else
        {
            stepRc = cap_capture_step(handle_);
        }

        if (stepRc == CAP_OK)
            continue;

        // 如果已經在 stop 過程，直接退出
        if (!running_)
            break;

        break;
    } });

    return true;
}

void CaptureSdkSource::stop()
{
    if (!handle_)
        return;

    if (capturing_)
    {
        running_ = false;

        if (stepTimeout_)
        {
            // With timeout API, the pump thread will exit quickly.
            if (pumpThread_.joinable())
                pumpThread_.join();

            cap_stop_capture(handle_);
            capturing_ = false;
        }
        else
        {
            // Best-effort to unblock a potentially blocking cap_capture_step().
            cap_stop_capture(handle_);
            capturing_ = false;
            cap_uninit(handle_);

            if (pumpThread_.joinable())
                pumpThread_.join();
        }
        cap_set_log_callback(nullptr);
        g_log_receiver = nullptr;
    }

    // If we already uninit'ed in the no-timeout path above, this is a no-op.
    cap_uninit(handle_);

    cap_destroy(handle_);
    handle_ = nullptr;
}

void __stdcall CaptureSdkSource::s_video_cb(const uint8_t *buf,
                                            int width,
                                            int height,
                                            int bytes_per_pixel,
                                            void *user)
{
    // NOTE: Do NOT log every frame; it can easily stall the capture pipeline.
    static std::atomic<uint64_t> cbCount{0};
    static std::atomic<long long> lastLogUs{0};
    auto n = ++cbCount;

    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    const long long us = std::chrono::duration_cast<std::chrono::microseconds>(now).count();
    const long long prev = lastLogUs.load(std::memory_order_relaxed);
    if (n == 1 || (us - prev) > 1'000'000)
    {
        lastLogUs.store(us, std::memory_order_relaxed);
        qDebug() << "[cap] video_cb n=" << n
                 << " w=" << width << " h=" << height
                 << " bpp=" << bytes_per_pixel
                 << " buf=" << (const void *)buf;
    }

    auto *self = reinterpret_cast<CaptureSdkSource *>(user);
    if (!self)
        return;

    self->onVideo(buf, width, height, bytes_per_pixel);
}

void CaptureSdkSource::onVideo(const uint8_t *buf, int width, int height, int bpp)
{
    if (!buf || width <= 0 || height <= 0)
        return;

    // We intentionally copy the frame: SDK buffer ownership/lifetime is unknown.
    QImage img;
    if (bpp == 4)
    {
        img = QImage(buf, width, height, width * 4, QImage::Format_RGBA8888).copy();
    }
    else if (bpp == 3)
    {
        img = QImage(buf, width, height, width * 3, QImage::Format_RGB888).copy();
    }
    else if (bpp == 2)
    {
        // Common for RGB565; still show something.
        img = QImage(buf, width, height, width * 2, QImage::Format_RGB16).copy();
    }
    else
    {
        // Unknown format; ignore.
        return;
    }

    emit frameReady(img);
}
