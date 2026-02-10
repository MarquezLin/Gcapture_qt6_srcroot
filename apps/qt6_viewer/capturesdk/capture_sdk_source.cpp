#include "capture_sdk_source.h"

#include <QCoreApplication>
#include <QDir>

#ifdef _WIN32
  #include <windows.h>
#endif

CaptureSdkSource::CaptureSdkSource(QObject *parent)
    : QObject(parent)
{
}

CaptureSdkSource::~CaptureSdkSource()
{
    stop();
    unloadDll();
}

bool CaptureSdkSource::isLoaded() const
{
#ifdef _WIN32
    return dll_ != nullptr;
#else
    return false;
#endif
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

static QString defaultDllPath()
{
    // Prefer <exe>/CaptureSDK.dll (CMake copies it).
    const QString exeDir = QCoreApplication::applicationDirPath();
    return QDir(exeDir).filePath(QStringLiteral("CaptureSDK.dll"));
}

bool CaptureSdkSource::loadDll()
{
#ifndef _WIN32
    setError(QStringLiteral("CaptureSDK backend is Windows-only."));
    return false;
#else
    if (dll_)
        return true;

    const QString dllPath = defaultDllPath();
    dll_ = ::LoadLibraryW(reinterpret_cast<LPCWSTR>(dllPath.utf16()));
    if (!dll_)
    {
        setError(QStringLiteral("LoadLibrary failed: %1").arg(dllPath));
        return false;
    }

    auto gp = [this](const char *name) -> FARPROC {
        return ::GetProcAddress(dll_, name);
    };

    cap_create_        = reinterpret_cast<cap_create_t>(gp("cap_create"));
    cap_destroy_       = reinterpret_cast<cap_destroy_t>(gp("cap_destroy"));
    cap_init_          = reinterpret_cast<cap_init_t>(gp("cap_init"));
    cap_uninit_        = reinterpret_cast<cap_uninit_t>(gp("cap_uninit"));
    cap_start_capture_ = reinterpret_cast<cap_start_capture_t>(gp("cap_start_capture"));
    cap_stop_capture_  = reinterpret_cast<cap_stop_capture_t>(gp("cap_stop_capture"));

    if (!cap_create_ || !cap_destroy_ || !cap_init_ || !cap_uninit_ || !cap_start_capture_ || !cap_stop_capture_)
    {
        setError(QStringLiteral("CaptureSDK.dll is missing required exports."));
        unloadDll();
        return false;
    }

    return true;
#endif
}

void CaptureSdkSource::unloadDll()
{
#ifdef _WIN32
    if (dll_)
    {
        ::FreeLibrary(dll_);
        dll_ = nullptr;
    }
#endif
    cap_create_ = nullptr;
    cap_destroy_ = nullptr;
    cap_init_ = nullptr;
    cap_uninit_ = nullptr;
    cap_start_capture_ = nullptr;
    cap_stop_capture_ = nullptr;
}

bool CaptureSdkSource::start(int width, int height)
{
#ifndef _WIN32
    Q_UNUSED(width)
    Q_UNUSED(height)
    setError(QStringLiteral("CaptureSDK backend is Windows-only."));
    return false;
#else
    if (capturing_)
        return true;

    if (!loadDll())
        return false;

    if (!handle_)
    {
        cap_handle_t h = nullptr;
        const int rc = cap_create_(&h);
        if (rc != 0 || !h)
        {
            setError(QStringLiteral("cap_create failed (%1)").arg(rc));
            return false;
        }
        handle_ = h;
    }

    // Try (0,0) as "SDK default" first if caller didn't specify.
    int w = width;
    int h = height;
    if (w < 0) w = 0;
    if (h < 0) h = 0;

    int rc = cap_init_(handle_, w, h);
    if (rc != 0)
    {
        // Fallback: a sane default if SDK doesn't accept 0,0.
        if (w == 0 && h == 0)
            rc = cap_init_(handle_, 1920, 1080);

        if (rc != 0)
        {
            setError(QStringLiteral("cap_init failed (%1)").arg(rc));
            return false;
        }
    }

    rc = cap_start_capture_(handle_, &CaptureSdkSource::s_video_cb, this);
    if (rc != 0)
    {
        setError(QStringLiteral("cap_start_capture failed (%1)").arg(rc));
        cap_uninit_(handle_);
        return false;
    }

    capturing_ = true;
    return true;
#endif
}

void CaptureSdkSource::stop()
{
#ifndef _WIN32
    return;
#else
    if (!handle_)
        return;

    if (capturing_)
    {
        cap_stop_capture_(handle_);
        capturing_ = false;
    }

    cap_uninit_(handle_);

    cap_destroy_(handle_);
    handle_ = nullptr;
#endif
}

void CaptureSdkSource::s_video_cb(const uint8_t *buf,
                                 int width,
                                 int height,
                                 int bytes_per_pixel,
                                 void *user)
{
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
