// src/providers/dshow_provider.h
#pragma once

#include <vector>
#include <string>
#include <mutex>
#include <atomic>
#include <thread>
#include <wrl/client.h>

#include <windows.h>
#include <dshow.h>
#include <d3d9.h>
#include <vmr9.h>

#include "gcapture.h"
#include "dshow_raw_renderer.h"
#include "dshow_custom_sink.h"
#include "../core/capture_manager.h"

class DShowProvider : public ICaptureProvider
{
public:
    DShowProvider();
    ~DShowProvider() override;

    bool enumerate(std::vector<gcap_device_info_t> &list) override;
    bool open(int index) override;
    bool setProfile(const gcap_profile_t &p) override;
    bool setBuffers(int count, size_t bytes_hint) override;
    bool start() override;
    void stop() override;
    void close() override;
    void setCallbacks(gcap_on_video_cb vcb, gcap_on_error_cb ecb, void *user) override;
    bool getSignalStatus(gcap_signal_status_t &out) override;
    bool getRuntimeInfo(gcap_runtime_info_t &out) override;
    bool setPreview(const gcap_preview_desc_t &desc) override;

private:
    void ensure_com();
    void uninit_com();
    bool buildGraphForDevice(int index);
    bool isRawOnlyMode() const;
    bool buildPreviewGraph(ICaptureGraphBuilder2 *capBuilder);
    bool buildRawOnlyGraph(ICaptureGraphBuilder2 *capBuilder);
    bool configureCaptureFormat(IAMStreamConfig *streamConfig);
    void logCaptureCapabilities(IAMStreamConfig *streamConfig);
    void updatePreviewRect();
    void startMirrorThread();
    void stopMirrorThread();
    void mirrorLoop();
    enum class CallbackSource
    {
        Unknown = 0,
        RawSink,
        RendererImage,
        PreviewBitBlt
    };

    bool captureRawFrameToArgb(std::vector<uint8_t> &out, int &w, int &h, int &stride);
    bool capturePreviewFrameToArgb(std::vector<uint8_t> &out, int &w, int &h, int &stride);
    bool captureRendererFrameToArgb(std::vector<uint8_t> &out, int &w, int &h, int &stride);
    bool captureCallbackFrameToArgb(std::vector<uint8_t> &out, int &w, int &h, int &stride);
    int mirrorSleepMs() const;
    bool isRawCandidate() const;
    const char *callbackSourceName(CallbackSource src) const;
    bool rawSinkPlanned() const;

private:
    Microsoft::WRL::ComPtr<IGraphBuilder> graph_;
    Microsoft::WRL::ComPtr<IMediaControl> mediaControl_;
    Microsoft::WRL::ComPtr<IMediaEvent> mediaEvent_;

    Microsoft::WRL::ComPtr<IBaseFilter> sourceFilter_;
    Microsoft::WRL::ComPtr<IBaseFilter> previewRenderer_;
    Microsoft::WRL::ComPtr<IBaseFilter> smartTee_;
    Microsoft::WRL::ComPtr<IVMRWindowlessControl9> vmrWindowless_;

    GUID subtype_ = MEDIASUBTYPE_NULL;
    int width_ = 0;
    int height_ = 0;
    int negotiatedFpsNum_ = 0;
    int negotiatedFpsDen_ = 0;

    std::atomic<bool> running_{false};
    int currentIndex_ = -1;

    gcap_profile_t profile_{};
    gcap_on_video_cb vcb_ = nullptr;
    gcap_on_error_cb ecb_ = nullptr;
    void *user_ = nullptr;

    std::mutex mtx_;
    std::vector<uint8_t> argbBuffer_;
    std::atomic<uint64_t> frameCounter_{0};
    std::atomic<CallbackSource> lastCallbackSource_{CallbackSource::Unknown};

    std::thread mirrorThread_;
    std::atomic<bool> mirrorThreadRunning_{false};
    HWND previewHwnd_ = nullptr;
    DShowRawRenderer rawRenderer_{};
    DShowCustomSinkFilter *rawSinkFilter_ = nullptr;
    bool rawOnlyActive_ = false;
};
