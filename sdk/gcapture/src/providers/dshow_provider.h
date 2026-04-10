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
#include <d3d11_1.h>
#include <d2d1_1.h>
#include <dwrite.h>
#include <memory>

#include "gcapture.h"
#include "dshow_raw_renderer.h"
#include "dshow_custom_sink.h"
#include "../core/capture_manager.h"
#include "../pipeline/shared_scene_pipeline.h"

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
    void setFramePacketCallback(gcap_on_frame_packet_cb pcb, void *user) override;
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
    void startFramePumpThread();
    void stopFramePumpThread();
    void framePumpLoop();
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
    int framePumpSleepMs() const;
    bool shouldDoSharedReadback(uint64_t ptsNs, uint64_t frameId, bool sharedReady, bool havePreview, bool haveCallback, uint64_t &lastReadbackPtsNs) const;
    int callbackTargetFps() const;
    bool isRawCandidate() const;
    const char *callbackSourceName(CallbackSource src) const;
    bool rawSinkPlanned() const;
    bool createRenderPipeline();
    void releaseRenderPipeline();
    void resetPreviewProbeStats();
    void logPreviewProbeStats(uint64_t frameId, int frameW, int frameH, bool directRaw, const char *presentTag);
    bool refreshSignalProbe(bool force);

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
    bool signalValid_ = false;
    int signalW_ = 0;
    int signalH_ = 0;
    int signalFpsNum_ = 0;
    int signalFpsDen_ = 1;
    GUID signalSubtype_ = GUID_NULL;
    bool signalHasSc0710CustomPage_ = false;
    wchar_t signalSc0710Module_[260] = {};
    uint64_t lastSignalProbeMs_ = 0;

    std::atomic<bool> running_{false};
    int currentIndex_ = -1;

    gcap_profile_t profile_{};
    gcap_on_video_cb vcb_ = nullptr;
    gcap_on_frame_packet_cb pcb_ = nullptr;
    gcap_on_error_cb ecb_ = nullptr;
    void *user_ = nullptr;

    std::mutex mtx_;
    std::vector<uint8_t> argbBuffer_;
    std::atomic<uint64_t> frameCounter_{0};
    std::atomic<CallbackSource> lastCallbackSource_{CallbackSource::Unknown};

    std::thread framePumpThread_;
    std::atomic<bool> framePumpThreadRunning_{false};
    HWND previewHwnd_ = nullptr;
    gcap_preview_desc_t previewDesc_{};
    DShowRawRenderer rawRenderer_{};
    DShowCustomSinkFilter *rawSinkFilter_ = nullptr;

    Microsoft::WRL::ComPtr<ID3D11Device> d3d_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> ctx_;
    Microsoft::WRL::ComPtr<ID2D1Factory1> d2d_factory_;
    Microsoft::WRL::ComPtr<ID2D1Device> d2d_device_;
    Microsoft::WRL::ComPtr<ID2D1DeviceContext> d2d_ctx_;
    Microsoft::WRL::ComPtr<IDWriteFactory> dwrite_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> d2d_white_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> d2d_black_;
    std::unique_ptr<SharedScenePipeline> pipeline_;

    struct PreviewProbeStats
    {
        uint64_t frames = 0;
        uint64_t copyLatestRawNs = 0;
        uint64_t ensureRtNs = 0;
        uint64_t ensureSwapNs = 0;
        uint64_t uploadNs = 0;
        uint64_t renderYuvNs = 0;
        uint64_t copySceneNs = 0;
        uint64_t blitNs = 0;
        uint64_t presentNs = 0;
        uint64_t readbackNs = 0;
        uint64_t readbackFrames = 0;
        uint64_t callbackFrames = 0;
    } previewProbeStats_;

    bool rawOnlyActive_ = false;
    // Preview active: use low-frequency ARGB callback to coexist with smooth preview.
    // No preview active: callback path may still run per-frame.
    int callbackTargetFps_ = 10;
};
