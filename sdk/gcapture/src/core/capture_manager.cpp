#include "capture_manager.h"
#include <cstring>

#ifdef GCAP_WIN_MF
#include "../providers/winmf_provider.h"
#endif

#ifdef GCAP_WIN_DSHOW
#include "../providers/dshow_provider.h"
#endif

enum class Backend
{
    WinMF_CPU,
    WinMF_GPU,
    DShow,
    Auto
};
static Backend g_backend = Backend::WinMF_GPU;

// 預設 D3D Adapter (-1 = 由系統選擇 default adapter)
static int g_d3d_adapter_index = -1;

/**
 * @brief Constructor — selects platform-specific provider.
 */
CaptureManager::CaptureManager()
{
    switch (g_backend)
    {
    case Backend::WinMF_CPU:
        selectedBackendInt_ = GCAP_BACKEND_WINMF_CPU;
        break;
    case Backend::DShow:
        selectedBackendInt_ = GCAP_BACKEND_DSHOW;
        break;
    case Backend::Auto:
        selectedBackendInt_ = GCAP_BACKEND_AUTO;
        break;
    case Backend::WinMF_GPU:
    default:
        selectedBackendInt_ = GCAP_BACKEND_WINMF_GPU;
        break;
    }

    activeBackendInt_ = selectedBackendInt_ == GCAP_BACKEND_AUTO
                            ? GCAP_BACKEND_WINMF_GPU
                            : selectedBackendInt_;
    rebuildProviderForBackend(activeBackendInt_);
}

bool CaptureManager::rebuildProviderForBackend(int backendInt)
{
    provider_.reset();

    switch (backendInt)
    {
#ifdef GCAP_WIN_DSHOW
    case GCAP_BACKEND_DSHOW:
        provider_ = std::make_unique<DShowProvider>();
        activeBackendInt_ = GCAP_BACKEND_DSHOW;
        return true;
#endif
#ifdef GCAP_WIN_MF
    case GCAP_BACKEND_WINMF_CPU:
        provider_ = std::make_unique<WinMFProvider>(false);
        activeBackendInt_ = GCAP_BACKEND_WINMF_CPU;
        return true;
    case GCAP_BACKEND_WINMF_GPU:
        provider_ = std::make_unique<WinMFProvider>(true);
        activeBackendInt_ = GCAP_BACKEND_WINMF_GPU;
        return true;
#endif
    default:
        break;
    }

    activeBackendInt_ = backendInt;
    return false;
}

bool CaptureManager::applyCachedStateToProvider()
{
    if (!provider_)
        return false;

    provider_->setCallbacks(vcb_, ecb_, user_);
    provider_->setFramePacketCallback(pcb_, user_);

    if (hasProfile_ && !provider_->setProfile(cachedProfile_))
        return false;

    if (!provider_->setBuffers(cachedBufferCount_, cachedBufferBytesHint_))
        return false;

    if (hasPreview_ && !provider_->setPreview(cachedPreview_))
        return false;

    return true;
}

bool CaptureManager::openWithBackend(int backendInt, int deviceIndex)
{
    if (!rebuildProviderForBackend(backendInt) || !provider_)
        return false;

    if (!applyCachedStateToProvider())
        return false;

    if (!provider_->open(deviceIndex))
        return false;

    openedDeviceIndex_ = deviceIndex;

    // open 後再補一次設定，讓某些 backend 可以把設定真正套到已開啟裝置上。
    return applyCachedStateToProvider();
}

/**
 * @brief Destructor — ensures the device is properly closed.
 */
CaptureManager::~CaptureManager()
{
    close();
}

void CaptureManager::setBackendInt(int v)
{
    switch (v)
    {
    case GCAP_BACKEND_WINMF_CPU:
        g_backend = Backend::WinMF_CPU;
        break;
    case GCAP_BACKEND_WINMF_GPU:
        g_backend = Backend::WinMF_GPU;
        break;
    case GCAP_BACKEND_DSHOW:
        g_backend = Backend::DShow;
        break;
    case GCAP_BACKEND_AUTO:
        g_backend = Backend::Auto;
        break;
    default:
        g_backend = Backend::WinMF_GPU;
        break;
    }
}

void CaptureManager::setD3dAdapterInt(int index)
{
    g_d3d_adapter_index = index;

#ifdef GCAP_WIN_MF
    // 告訴 WinMFProvider 要改用哪一張 GPU
    WinMFProvider::setPreferredAdapterIndex(index);
#else
    (void)index;
#endif
}

/**
 * @brief Enumerate all capture devices.
 * @return GCAP_OK if successful, or error code otherwise.
 */
gcap_status_t CaptureManager::enumerate(gcap_device_info_t *out, int max, int *count)
{
    if (!provider_)
        return GCAP_ENOTSUP; // Not supported on this platform
    std::vector<gcap_device_info_t> list;
    if (!provider_->enumerate(list))
        return GCAP_EIO;
    int n = (int)list.size();
    if (count)
        *count = n;
    for (int i = 0; i < n && i < max; ++i)
        out[i] = list[i];
    return GCAP_OK;
}

/**
 * @brief Open the selected device.
 */
gcap_status_t CaptureManager::open(int idx)
{
    if (selectedBackendInt_ == GCAP_BACKEND_AUTO)
    {
        const int candidates[] = {GCAP_BACKEND_WINMF_GPU, GCAP_BACKEND_WINMF_CPU, GCAP_BACKEND_DSHOW};
        for (int backendInt : candidates)
        {
            if (openWithBackend(backendInt, idx))
                return GCAP_OK;
        }
        return GCAP_EIO;
    }

    if (!openWithBackend(selectedBackendInt_, idx))
        return GCAP_EIO;

    return GCAP_OK;
}

/**
 * @brief Set the desired capture profile (resolution, FPS, format).
 */
gcap_status_t CaptureManager::setProfile(const gcap_profile_t &p)
{
    cachedProfile_ = p;
    hasProfile_ = true;
    if (!provider_)
        return GCAP_ENOTSUP;
    return provider_->setProfile(p) ? GCAP_OK : GCAP_EINVAL;
}

/**
 * @brief Configure capture buffers.
 */
gcap_status_t CaptureManager::setBuffers(int c, size_t b)
{
    cachedBufferCount_ = c;
    cachedBufferBytesHint_ = b;
    if (!provider_)
        return GCAP_ENOTSUP;
    return provider_->setBuffers(c, b) ? GCAP_OK : GCAP_EINVAL;
}

/**
 * @brief Register video and error callbacks.
 */
gcap_status_t CaptureManager::setCallbacks(gcap_on_video_cb v, gcap_on_error_cb e, void *u)
{
    vcb_ = v;
    ecb_ = e;
    user_ = u;
    if (!provider_)
        return GCAP_ENOTSUP;
    provider_->setCallbacks(vcb_, ecb_, user_);
    provider_->setFramePacketCallback(pcb_, user_);
    return GCAP_OK;
}

gcap_status_t CaptureManager::setFramePacketCallback(gcap_on_frame_packet_cb cb, void *u)
{
    pcb_ = cb;
    user_ = u;
    if (!provider_)
        return GCAP_ENOTSUP;
    provider_->setCallbacks(vcb_, ecb_, user_);
    provider_->setFramePacketCallback(pcb_, user_);
    return GCAP_OK;
}

/**
 * @brief Start video capture.
 */
gcap_status_t CaptureManager::start()
{
    if (!provider_)
        return GCAP_ENOTSUP;

    if (provider_->start())
        return GCAP_OK;

    if (selectedBackendInt_ == GCAP_BACKEND_AUTO && openedDeviceIndex_ >= 0)
    {
        const int current = activeBackendInt_;
        const int candidates[] = {GCAP_BACKEND_WINMF_GPU, GCAP_BACKEND_WINMF_CPU, GCAP_BACKEND_DSHOW};
        for (int backendInt : candidates)
        {
            if (backendInt == current)
                continue;
            if (!openWithBackend(backendInt, openedDeviceIndex_))
                continue;
            if (provider_->start())
                return GCAP_OK;
        }
    }

    return GCAP_ESTATE;
}

/**
 * @brief Stop video capture.
 */
gcap_status_t CaptureManager::stop()
{
    if (!provider_)
        return GCAP_ENOTSUP;
    provider_->stop();
    return GCAP_OK;
}

gcap_status_t CaptureManager::startRecording(const char *pathUtf8)
{
    if (!provider_)
        return GCAP_ENOTSUP;

#ifdef GCAP_WIN_MF
    if (auto *p = dynamic_cast<WinMFProvider *>(provider_.get()))
        return p->startRecording(pathUtf8);
#endif
    return GCAP_ENOTSUP;
}

gcap_status_t CaptureManager::stopRecording()
{
    if (!provider_)
        return GCAP_ENOTSUP;

#ifdef GCAP_WIN_MF
    if (auto *p = dynamic_cast<WinMFProvider *>(provider_.get()))
        return p->stopRecording();
#endif
    return GCAP_ENOTSUP;
}

gcap_status_t CaptureManager::setRecordingAudioDevice(const char *deviceIdUtf8)
{
    if (!provider_)
        return GCAP_ENOTSUP;

#ifdef GCAP_WIN_MF
    if (auto *p = dynamic_cast<WinMFProvider *>(provider_.get()))
        return p->setRecordingAudioDevice(deviceIdUtf8);
#endif
    (void)deviceIdUtf8;
    return GCAP_ENOTSUP;
}

/**
 * @brief Close the current device and release resources.
 */
gcap_status_t CaptureManager::close()
{
    if (!provider_)
        return GCAP_ENOTSUP;
    provider_->close();
    openedDeviceIndex_ = -1;
    return GCAP_OK;
}

gcap_status_t CaptureManager::getDeviceProps(gcap_device_props_t &out)
{
    if (!provider_)
        return GCAP_ENOTSUP;
    return provider_->getDeviceProps(out) ? GCAP_OK : GCAP_ENOTSUP;
}

gcap_status_t CaptureManager::getSignalStatus(gcap_signal_status_t &out)
{
    if (!provider_)
        return GCAP_ENOTSUP;
    return provider_->getSignalStatus(out) ? GCAP_OK : GCAP_ENOTSUP;
}

gcap_status_t CaptureManager::getRuntimeInfo(gcap_runtime_info_t &out)
{
    if (!provider_)
        return GCAP_ENOTSUP;
    return provider_->getRuntimeInfo(out) ? GCAP_OK : GCAP_ENOTSUP;
}

gcap_status_t CaptureManager::setProcessing(const gcap_processing_opts_t &opts)
{
    if (!provider_)
        return GCAP_ENOTSUP;
    return provider_->setProcessing(opts) ? GCAP_OK : GCAP_ENOTSUP;
}

gcap_status_t CaptureManager::setProcAmp(const gcap_procamp_t &p)
{
    if (!provider_)
        return GCAP_ENOTSUP;
    return provider_->setProcAmp(p) ? GCAP_OK : GCAP_ENOTSUP;
}

gcap_status_t CaptureManager::setPreview(const gcap_preview_desc_t &desc)
{
    cachedPreview_ = desc;
    hasPreview_ = true;
    if (!provider_)
        return GCAP_ESTATE;

    return provider_->setPreview(desc) ? GCAP_OK : GCAP_ENOTSUP;
}

int CaptureManager::getActiveBackendInt() const
{
    return activeBackendInt_;
}