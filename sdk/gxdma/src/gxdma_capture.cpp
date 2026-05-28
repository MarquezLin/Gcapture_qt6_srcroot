#include "gxdma_capture.h"

#include "gcapture.h"
#include "gvendor.h"
#include "pipeline/shared_scene_pipeline.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

#include <d2d1.h>
#include <d3d11_4.h>
#include <dxgi.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace
{
    void copy_cstr(char *dst, size_t dstSize, const char *src)
    {
        if (!dst || dstSize == 0)
            return;
        dst[0] = 0;
        if (src)
            strncpy_s(dst, dstSize, src, _TRUNCATE);
    }

    uint64_t now_ns()
    {
        const auto now = std::chrono::steady_clock::now().time_since_epoch();
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
    }

    gxdma_status_t map_status(gv_status_t st)
    {
        switch (st)
        {
        case GV_OK: return GXDMA_OK;
        case GV_EINVAL: return GXDMA_EINVAL;
        case GV_ENODEV: return GXDMA_ENODEV;
        case GV_ESTATE: return GXDMA_ESTATE;
        case GV_ETIMEOUT: return GXDMA_ETIMEOUT;
        case GV_ENOTSUP: return GXDMA_ENOTSUP;
        case GV_EIO:
        default: return GXDMA_EIO;
        }
    }

    const char *gv_error_text(gv_status_t st, gv_handle h)
    {
        const char *detail = h ? gv_last_error(h) : nullptr;
        if (detail && detail[0])
            return detail;
        return gv_strerror(st);
    }

    const char *dxgi_format_name(DXGI_FORMAT fmt)
    {
        switch (fmt)
        {
        case DXGI_FORMAT_UNKNOWN: return "UNKNOWN";
        case DXGI_FORMAT_B8G8R8A8_UNORM: return "B8G8R8A8_UNORM";
        case DXGI_FORMAT_R10G10B10A2_UNORM: return "R10G10B10A2_UNORM";
        case DXGI_FORMAT_R16G16B16A16_FLOAT: return "R16G16B16A16_FLOAT";
        default: return "DXGI_FORMAT_OTHER";
        }
    }

    int clamp_u8(int v)
    {
        return (std::max)(0, (std::min)(255, v));
    }

    void yuv_to_rgb(int y, int u, int v, uint8_t &r, uint8_t &g, uint8_t &b)
    {
        const int c = y - 16;
        const int d = u - 128;
        const int e = v - 128;
        r = static_cast<uint8_t>(clamp_u8((298 * c + 409 * e + 128) >> 8));
        g = static_cast<uint8_t>(clamp_u8((298 * c - 100 * d - 208 * e + 128) >> 8));
        b = static_cast<uint8_t>(clamp_u8((298 * c + 516 * d + 128) >> 8));
    }
}

struct gxdma_handle_t
{
    ~gxdma_handle_t()
    {
        close();
    }

    gxdma_status_t open(int index)
    {
        close();

        const gv_status_t stOpen = gv_open_device_index(index, &handle);
        if (stOpen != GV_OK || !handle)
        {
            emitError(map_status(stOpen), gv_error_text(stOpen, handle));
            return map_status(stOpen);
        }

        currentIndex = index;
        const gv_status_t stInput = gv_set_input(handle, GDRIVER_INPUT_SDI, 0);
        if (stInput != GV_OK)
        {
            emitError(map_status(stInput), gv_error_text(stInput, handle));
            close();
            return map_status(stInput);
        }

        querySignal();
        return GXDMA_OK;
    }

    gxdma_status_t start()
    {
        if (!handle)
            return GXDMA_ESTATE;
        if (running)
            return GXDMA_OK;

        const gxdma_status_t cfg = configureStream();
        if (cfg != GXDMA_OK)
            return cfg;

        if (previewHwnd && !createRenderPipeline())
            emitError(GXDMA_ENOTSUP, "XDMA preview pipeline unavailable");

        const gv_status_t st = gv_start_stream(handle);
        if (st != GV_OK)
        {
            emitError(map_status(st), gv_error_text(st, handle));
            return map_status(st);
        }

        running = true;
        captureThread = std::thread([this]() { captureLoop(); });
        return GXDMA_OK;
    }

    gxdma_status_t stop()
    {
        running = false;
        if (captureThread.joinable())
            captureThread.join();
        if (handle)
            gv_stop_stream(handle);
        return GXDMA_OK;
    }

    void close()
    {
        stop();
        releaseRenderPipeline();
        if (handle)
        {
            gv_close(handle);
            handle = nullptr;
        }
        currentIndex = -1;
    }

    gxdma_status_t setPreview(const gxdma_preview_desc_t &desc)
    {
        previewDesc = desc;
        previewHwnd = desc.enable_preview ? desc.hwnd : nullptr;
        if (pipeline)
            pipeline->configurePreview(toGcapPreviewDesc());
        if (previewHwnd && width > 0 && height > 0)
            createRenderPipeline();
        return GXDMA_OK;
    }

    gxdma_status_t getSignalStatus(gxdma_signal_status_t &out)
    {
        std::memset(&out, 0, sizeof(out));
        querySignal();
        out.width = static_cast<int>(width);
        out.height = static_cast<int>(height);
        out.fps_num = static_cast<int>(fpsNum);
        out.fps_den = static_cast<int>(fpsDen ? fpsDen : 1);
        out.bit_depth = static_cast<int>(bitDepth ? bitDepth : 8);
        copy_cstr(out.pixel_format, sizeof(out.pixel_format), "YUY2");
        return (out.width > 0 && out.height > 0) ? GXDMA_OK : GXDMA_ENODEV;
    }

    gxdma_status_t getRuntimeInfo(gxdma_runtime_info_t &out)
    {
        std::memset(&out, 0, sizeof(out));
        getSignalStatus(out.signal);
        out.runtime_fps = runtimeFps;
        out.delivered_frames = deliveredFrames;
        copy_cstr(out.backend_name, sizeof(out.backend_name), "GXDMA");
        copy_cstr(out.capture_path, sizeof(out.capture_path), "XDMA C2H -> YUY2 frame callback");
        copy_cstr(out.frame_source, sizeof(out.frame_source), "XDMA C2H");
        copy_cstr(out.source_format, sizeof(out.source_format), "YUY2");
        return GXDMA_OK;
    }

    gxdma_status_t getPreviewInfo(gxdma_preview_info_t &out)
    {
        std::memset(&out, 0, sizeof(out));
        out.enabled = previewHwnd ? 1 : 0;
        out.active = (pipeline && previewHwnd) ? 1 : 0;
        out.width = pipeline ? pipeline->preview_w_ : 0;
        out.height = pipeline ? pipeline->preview_h_ : 0;
        out.swapchain_bitdepth = pipeline ? (pipeline->preview_swapchain_10bit() ? 10 : 8)
                                          : previewDesc.swapchain_bitdepth;
        out.swapchain_10bit = pipeline && pipeline->preview_swapchain_10bit() ? 1 : 0;
        copy_cstr(out.render_path, sizeof(out.render_path),
                  pipeline ? (pipeline->preview_swapchain_10bit()
                                  ? "YUY2 Shader -> FP16 Scene -> 10bit Swapchain"
                                  : "YUY2 Shader -> FP16 Scene -> 8bit Swapchain")
                           : (previewHwnd ? "Preview helper configured, pipeline inactive"
                                          : "Preview helper disabled"));
        copy_cstr(out.backbuffer_format, sizeof(out.backbuffer_format),
                  pipeline ? dxgi_format_name(pipeline->preview_backbuffer_format()) : "N/A");
        return GXDMA_OK;
    }

    void querySignal()
    {
        if (!handle)
            return;

        gv_signal_status_t sig{};
        if (gv_get_signal_status(handle, &sig) != GV_OK)
            return;

        if (sig.width > 0)
            width = sig.width;
        if (sig.height > 0)
            height = sig.height;
        if (sig.fps_num > 0)
            fpsNum = sig.fps_num;
        if (sig.fps_den > 0)
            fpsDen = sig.fps_den;
        if (sig.bit_depth > 0)
            bitDepth = sig.bit_depth;
    }

    gxdma_status_t configureStream()
    {
        if (!handle)
            return GXDMA_ESTATE;

        querySignal();
        if (width == 0)
            width = 1920;
        if (height == 0)
            height = 1080;

        gv_stream_desc_t desc{};
        desc.channel_index = 0;
        desc.input = GDRIVER_INPUT_SDI;
        desc.width = width;
        desc.height = height;
        desc.fps_num = fpsNum ? fpsNum : 30000;
        desc.fps_den = fpsDen ? fpsDen : 1001;
        desc.pixel_format = GDRIVER_PIXFMT_YUY2;
        desc.buffer_count = 1;
        desc.memory_kind = GDRIVER_MEMORY_DRIVER_COPY;

        const gv_status_t st = gv_configure_stream(handle, &desc);
        if (st != GV_OK)
        {
            emitError(map_status(st), gv_error_text(st, handle));
            return map_status(st);
        }
        return GXDMA_OK;
    }

    gcap_preview_desc_t toGcapPreviewDesc() const
    {
        gcap_preview_desc_t desc{};
        desc.hwnd = previewHwnd;
        desc.enable_preview = previewHwnd ? 1 : 0;
        desc.use_fp16_pipeline = 1;
        switch (previewDesc.swapchain_bitdepth)
        {
        case GXDMA_PREVIEW_BITDEPTH_8BIT:
            desc.swapchain_10bit = GCAP_PREVIEW_BITDEPTH_8BIT;
            break;
        case GXDMA_PREVIEW_BITDEPTH_10BIT:
            desc.swapchain_10bit = GCAP_PREVIEW_BITDEPTH_10BIT;
            break;
        case GXDMA_PREVIEW_BITDEPTH_AUTO:
        default:
            desc.swapchain_10bit = GCAP_PREVIEW_BITDEPTH_AUTO;
            break;
        }
        return desc;
    }

    bool createRenderPipeline()
    {
        if (!previewHwnd || width == 0 || height == 0)
            return false;

        if (!pipeline)
            pipeline = std::make_unique<SharedScenePipeline>();

        if (!d3d)
        {
            UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifdef _DEBUG
            flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
            D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
            D3D_FEATURE_LEVEL got{};
            HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                           flags, levels, _countof(levels), D3D11_SDK_VERSION,
                                           &d3d, &got, &ctx);
#ifdef _DEBUG
            if (FAILED(hr))
            {
                flags &= ~D3D11_CREATE_DEVICE_DEBUG;
                hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                       flags, levels, _countof(levels), D3D11_SDK_VERSION,
                                       &d3d, &got, &ctx);
            }
#endif
            if (FAILED(hr) || !d3d || !ctx)
                return false;

            ComPtr<ID3D11Multithread> mt;
            if (SUCCEEDED(d3d.As(&mt)) && mt)
                mt->SetMultithreadProtected(TRUE);

            if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_MULTI_THREADED, d2dFactory.ReleaseAndGetAddressOf())) || !d2dFactory)
                return false;

            ComPtr<IDXGIDevice> dxgiDev;
            if (FAILED(d3d.As(&dxgiDev)) || !dxgiDev)
                return false;
            if (FAILED(d2dFactory->CreateDevice(dxgiDev.Get(), &d2dDevice)) || !d2dDevice)
                return false;
            if (FAILED(d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &d2dCtx)) || !d2dCtx)
                return false;
            if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), &dwrite)) || !dwrite)
                return false;
            d2dCtx->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &d2dWhite);
            d2dCtx->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 0.55f), &d2dBlack);
        }

        if (!pipeline->initialize(d3d.Get(), ctx.Get(), d2dCtx.Get(), dwrite.Get(), d2dWhite.Get(), d2dBlack.Get()))
            return false;

        pipeline->configurePreview(toGcapPreviewDesc());
        pipeline->set_source_bit_depth(static_cast<int>(bitDepth ? bitDepth : 8));
        return pipeline->ensure_rt_and_pipeline(static_cast<int>(width), static_cast<int>(height)) &&
               pipeline->ensure_preview_swapchain(static_cast<int>(width), static_cast<int>(height));
    }

    void releaseRenderPipeline()
    {
        if (pipeline)
        {
            pipeline->release_preview_swapchain();
            pipeline.reset();
        }
        d2dBlack.Reset();
        d2dWhite.Reset();
        dwrite.Reset();
        d2dCtx.Reset();
        d2dDevice.Reset();
        d2dFactory.Reset();
        ctx.Reset();
        d3d.Reset();
    }

    void captureLoop()
    {
        while (running)
        {
            gv_frame_t frame{};
            const gv_status_t st = gv_wait_frame(handle, 1000, &frame);
            if (!running)
                break;
            if (st == GV_ETIMEOUT)
                continue;
            if (st != GV_OK)
            {
                emitError(map_status(st), gv_error_text(st, handle));
                break;
            }

            updateRuntimeFps(frame.timestamp_ns ? frame.timestamp_ns : now_ns());
            ++deliveredFrames;
            const bool rendered = renderYuy2Frame(frame);
            if (rendered)
                emitReadbackFrame(frame);
            else
                emitCpuFallbackFrame(frame);
            gv_release_frame(handle, &frame);
        }
    }

    bool renderYuy2Frame(const gv_frame_t &frame)
    {
        if (!pipeline || !previewHwnd || frame.pixel_format != GDRIVER_PIXFMT_YUY2 || !frame.data)
            return false;

        const int w = static_cast<int>(frame.width);
        const int h = static_cast<int>(frame.height);
        const int stride = frame.plane_stride_bytes[0] ? static_cast<int>(frame.plane_stride_bytes[0]) : w * 2;
        if (w <= 0 || h <= 0 || stride < w * 2)
            return false;

        if (!pipeline->ensure_rt_and_pipeline(w, h) ||
            !pipeline->ensure_preview_swapchain(w, h) ||
            !pipeline->upload_yuy2_frame(static_cast<const uint8_t *>(frame.data), stride, w, h) ||
            !pipeline->render_uploaded_yuv_to_fp16(GCAP_FMT_YUY2, w, h) ||
            !pipeline->copy_fp16_to_scene())
            return false;

        bool ok = true;
        if (!pipeline->preview_swapchain_10bit())
            ok = pipeline->blit_fp16_to_rgba8(w, h);
        if (ok)
            pipeline->present_preview(w, h);
        return ok;
    }

    void emitReadbackFrame(const gv_frame_t &frame)
    {
        if (!onFrame || !pipeline || !ctx)
            return;
        if ((frame.frame_id % 6) != 1)
            return;

        const int w = static_cast<int>(frame.width);
        const int h = static_cast<int>(frame.height);
        if (!pipeline->blit_fp16_to_rgba8(w, h))
            return;

        gcap_frame_t readback{};
        if (pipeline->readback_to_frame(w, h, frame.timestamp_ns ? frame.timestamp_ns : now_ns(), frame.frame_id, &readback))
        {
            gxdma_frame_t out{};
            out.data = readback.data[0];
            out.stride = readback.stride[0];
            out.width = readback.width;
            out.height = readback.height;
            out.pts_ns = readback.pts_ns;
            out.frame_id = readback.frame_id;
            onFrame(&out, callbackUser);
            ctx->Unmap(pipeline->rt_stage_.Get(), 0);
        }
    }

    void emitCpuFallbackFrame(const gv_frame_t &frame)
    {
        if (!onFrame || frame.pixel_format != GDRIVER_PIXFMT_YUY2 || !frame.data)
            return;
        if ((frame.frame_id % 6) != 1)
            return;

        const int w = static_cast<int>(frame.width);
        const int h = static_cast<int>(frame.height);
        const int srcStride = frame.plane_stride_bytes[0] ? static_cast<int>(frame.plane_stride_bytes[0]) : w * 2;
        if (w <= 0 || h <= 0 || srcStride < w * 2)
            return;

        if (!fallbackLogged)
        {
            emitError(GXDMA_ENOTSUP, "GPU preview/readback unavailable; using CPU YUY2 preview fallback");
            fallbackLogged = true;
        }

        fallbackBgra.resize(static_cast<size_t>(w) * static_cast<size_t>(h) * 4u);
        const auto *srcBase = static_cast<const uint8_t *>(frame.data);
        for (int y = 0; y < h; ++y)
        {
            const uint8_t *src = srcBase + static_cast<size_t>(y) * static_cast<size_t>(srcStride);
            uint8_t *dst = fallbackBgra.data() + static_cast<size_t>(y) * static_cast<size_t>(w) * 4u;
            for (int x = 0; x + 1 < w; x += 2)
            {
                const int y0 = src[x * 2 + 0];
                const int u = src[x * 2 + 1];
                const int y1 = src[x * 2 + 2];
                const int v = src[x * 2 + 3];

                uint8_t r0 = 0, g0 = 0, b0 = 0;
                uint8_t r1 = 0, g1 = 0, b1 = 0;
                yuv_to_rgb(y0, u, v, r0, g0, b0);
                yuv_to_rgb(y1, u, v, r1, g1, b1);

                dst[x * 4 + 0] = b0;
                dst[x * 4 + 1] = g0;
                dst[x * 4 + 2] = r0;
                dst[x * 4 + 3] = 255;
                dst[(x + 1) * 4 + 0] = b1;
                dst[(x + 1) * 4 + 1] = g1;
                dst[(x + 1) * 4 + 2] = r1;
                dst[(x + 1) * 4 + 3] = 255;
            }
        }

        gxdma_frame_t out{};
        out.data = fallbackBgra.data();
        out.stride = w * 4;
        out.width = w;
        out.height = h;
        out.pts_ns = frame.timestamp_ns ? frame.timestamp_ns : now_ns();
        out.frame_id = frame.frame_id;
        onFrame(&out, callbackUser);
    }

    void emitError(gxdma_status_t code, const char *msg)
    {
        if (onError)
            onError(code, msg ? msg : "", callbackUser);
    }

    void updateRuntimeFps(uint64_t ptsNs)
    {
        if (lastPtsNs != 0 && ptsNs > lastPtsNs)
        {
            const double fps = 1e9 / static_cast<double>(ptsNs - lastPtsNs);
            if (fps > 0.0 && fps < 1000.0)
                runtimeFps = (runtimeFps <= 0.0) ? fps : runtimeFps * 0.9 + fps * 0.1;
        }
        lastPtsNs = ptsNs;
    }

    gv_handle handle = nullptr;
    int currentIndex = -1;
    gxdma_preview_desc_t previewDesc{};
    void *previewHwnd = nullptr;

    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t fpsNum = 30000;
    uint32_t fpsDen = 1001;
    uint32_t bitDepth = 8;
    uint64_t lastPtsNs = 0;
    uint64_t deliveredFrames = 0;
    double runtimeFps = 0.0;

    gxdma_on_frame_cb onFrame = nullptr;
    gxdma_on_error_cb onError = nullptr;
    void *callbackUser = nullptr;
    bool fallbackLogged = false;
    std::vector<uint8_t> fallbackBgra;

    std::atomic<bool> running{false};
    std::thread captureThread;

    ComPtr<ID3D11Device> d3d;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<ID2D1Factory1> d2dFactory;
    ComPtr<ID2D1Device> d2dDevice;
    ComPtr<ID2D1DeviceContext> d2dCtx;
    ComPtr<IDWriteFactory> dwrite;
    ComPtr<ID2D1SolidColorBrush> d2dWhite;
    ComPtr<ID2D1SolidColorBrush> d2dBlack;
    std::unique_ptr<SharedScenePipeline> pipeline;
};

extern "C"
{
    int gxdma_enumerate_devices(gxdma_device_info_t *out_devices, int max_devices)
    {
        gv_device_entry_t entries[GXDMA_MAX_DEVICES] = {};
        const int maxCount = static_cast<int>(GXDMA_MAX_DEVICES);
        const int cap = (std::min)(max_devices > 0 ? max_devices : maxCount, maxCount);
        const int n = gv_enumerate_devices(entries, cap);
        if (n <= 0)
            return n;

        if (!out_devices || max_devices <= 0)
            return n;

        const int written = (std::min)(n, max_devices);
        for (int i = 0; i < written; ++i)
        {
            out_devices[i] = {};
            out_devices[i].index = i;
            copy_cstr(out_devices[i].name, sizeof(out_devices[i].name),
                      entries[i].friendly_name[0] ? entries[i].friendly_name : "XDMA Capture");
            copy_cstr(out_devices[i].path, sizeof(out_devices[i].path), entries[i].device_path);
        }
        return written;
    }

    gxdma_status_t gxdma_create(gxdma_handle *out_handle)
    {
        if (!out_handle)
            return GXDMA_EINVAL;
        auto h = std::make_unique<gxdma_handle_t>();
        *out_handle = h.release();
        return GXDMA_OK;
    }

    gxdma_status_t gxdma_destroy(gxdma_handle handle)
    {
        delete handle;
        return GXDMA_OK;
    }

    gxdma_status_t gxdma_set_callbacks(gxdma_handle handle,
                                       gxdma_on_frame_cb on_frame,
                                       gxdma_on_error_cb on_error,
                                       void *user)
    {
        if (!handle)
            return GXDMA_EINVAL;
        handle->onFrame = on_frame;
        handle->onError = on_error;
        handle->callbackUser = user;
        return GXDMA_OK;
    }

    gxdma_status_t gxdma_set_preview(gxdma_handle handle, const gxdma_preview_desc_t *desc)
    {
        if (!handle || !desc)
            return GXDMA_EINVAL;
        return handle->setPreview(*desc);
    }

    gxdma_status_t gxdma_open(gxdma_handle handle, int device_index)
    {
        if (!handle)
            return GXDMA_EINVAL;
        return handle->open(device_index);
    }

    gxdma_status_t gxdma_start(gxdma_handle handle)
    {
        if (!handle)
            return GXDMA_EINVAL;
        return handle->start();
    }

    gxdma_status_t gxdma_stop(gxdma_handle handle)
    {
        if (!handle)
            return GXDMA_EINVAL;
        return handle->stop();
    }

    gxdma_status_t gxdma_get_signal_status(gxdma_handle handle, gxdma_signal_status_t *out_status)
    {
        if (!handle || !out_status)
            return GXDMA_EINVAL;
        return handle->getSignalStatus(*out_status);
    }

    gxdma_status_t gxdma_get_runtime_info(gxdma_handle handle, gxdma_runtime_info_t *out_info)
    {
        if (!handle || !out_info)
            return GXDMA_EINVAL;
        return handle->getRuntimeInfo(*out_info);
    }

    gxdma_status_t gxdma_get_preview_info(gxdma_handle handle, gxdma_preview_info_t *out_info)
    {
        if (!handle || !out_info)
            return GXDMA_EINVAL;
        return handle->getPreviewInfo(*out_info);
    }

    const char *gxdma_strerror(gxdma_status_t status)
    {
        switch (status)
        {
        case GXDMA_OK: return "OK";
        case GXDMA_EINVAL: return "Invalid argument";
        case GXDMA_ENODEV: return "No XDMA device";
        case GXDMA_ESTATE: return "Invalid state";
        case GXDMA_EIO: return "I/O error";
        case GXDMA_ENOTSUP: return "Not supported";
        case GXDMA_ETIMEOUT: return "Timeout";
        default: return "Unknown";
        }
    }
}
