#include "gvfg_capture.h"

#include "gcapture.h"
#include "pipeline/shared_scene_pipeline.h"
#include "xdma_capture_session.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
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

    gvfg_status_t map_status(xdma_status_t st)
    {
        switch (st)
        {
        case XDMA_OK:
            return GVFG_OK;
        case XDMA_EINVAL:
            return GVFG_EINVAL;
        case XDMA_ENODEV:
            return GVFG_ENODEV;
        case XDMA_ESTATE:
            return GVFG_ESTATE;
        case XDMA_ETIMEOUT:
            return GVFG_ETIMEOUT;
        case XDMA_ENOTSUP:
            return GVFG_ENOTSUP;
        case XDMA_EIO:
        default:
            return GVFG_EIO;
        }
    }

    const char *xdma_status_text(xdma_status_t st)
    {
        switch (st)
        {
        case XDMA_OK:
            return "ok";
        case XDMA_EINVAL:
            return "invalid argument";
        case XDMA_ENODEV:
            return "device not found";
        case XDMA_ESTATE:
            return "invalid state";
        case XDMA_ENOTSUP:
            return "not supported";
        case XDMA_ETIMEOUT:
            return "timeout";
        case XDMA_EIO:
            return "i/o error";
        case XDMA_EABI:
            return "abi mismatch";
        default:
            return "unknown";
        }
    }

    const char *xdma_error_text(xdma_status_t st, const gvfg::internal::XdmaCaptureSession *session)
    {
        const char *detail = session ? session->last_error() : nullptr;
        if (detail && detail[0])
            return detail;
        return xdma_status_text(st);
    }

    void copy_wide_to_utf8(const std::wstring &src, char *dst, size_t dstSize)
    {
        if (!dst || dstSize == 0)
            return;
        dst[0] = '\0';
        if (src.empty())
            return;
        WideCharToMultiByte(CP_UTF8, 0, src.c_str(), -1, dst, static_cast<int>(dstSize), nullptr, nullptr);
        dst[dstSize - 1] = '\0';
    }

    gvfg_event_type_t map_event_type(xdma_event_type_t type)
    {
        switch (type)
        {
        case XDMA_EVENT_VIDEO_IRQ:
            return GVFG_EVENT_VIDEO_IRQ;
        case XDMA_EVENT_PLUG_IN:
            return GVFG_EVENT_PLUG_IN;
        case XDMA_EVENT_PLUG_OUT:
            return GVFG_EVENT_PLUG_OUT;
        case XDMA_EVENT_CAPTURE_PAUSED:
            return GVFG_EVENT_CAPTURE_PAUSED;
        case XDMA_EVENT_CAPTURE_RESUMED:
            return GVFG_EVENT_CAPTURE_RESUMED;
        default:
            return GVFG_EVENT_VIDEO_IRQ;
        }
    }

    uint32_t map_event_mask_to_xdma(uint32_t mask)
    {
        const uint32_t effective = mask ? mask : GVFG_EVENT_MASK_DEFAULT;
        uint32_t out = 0;
        if (effective & GVFG_EVENT_MASK_VIDEO_IRQ)
            out |= XDMA_EVENT_MASK_VIDEO_IRQ;
        if (effective & GVFG_EVENT_MASK_PLUG_IN)
            out |= XDMA_EVENT_MASK_PLUG_IN;
        if (effective & GVFG_EVENT_MASK_PLUG_OUT)
            out |= XDMA_EVENT_MASK_PLUG_OUT;
        if (effective & GVFG_EVENT_MASK_CAPTURE_PAUSED)
            out |= XDMA_EVENT_MASK_CAPTURE_PAUSED;
        if (effective & GVFG_EVENT_MASK_CAPTURE_RESUMED)
            out |= XDMA_EVENT_MASK_CAPTURE_RESUMED;
        return out ? out : XDMA_EVENT_MASK_DEFAULT;
    }

    const char *dxgi_format_name(DXGI_FORMAT fmt)
    {
        switch (fmt)
        {
        case DXGI_FORMAT_UNKNOWN:
            return "UNKNOWN";
        case DXGI_FORMAT_B8G8R8A8_UNORM:
            return "B8G8R8A8_UNORM";
        case DXGI_FORMAT_R10G10B10A2_UNORM:
            return "R10G10B10A2_UNORM";
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
            return "R16G16B16A16_FLOAT";
        default:
            return "DXGI_FORMAT_OTHER";
        }
    }

    const char *gdriver_pixel_format_name(gdriver_pixel_format_t fmt)
    {
        switch (fmt)
        {
        case GDRIVER_PIXFMT_YUY2:
            return "YUY2";
        case GDRIVER_PIXFMT_Y210:
            return "Y210";
        case GDRIVER_PIXFMT_RGB24:
            return "RGB24";
        case GDRIVER_PIXFMT_NV12:
            return "NV12";
        case GDRIVER_PIXFMT_P010:
            return "P010";
        case GDRIVER_PIXFMT_YUV444:
            return "YUV444";
        default:
            return "UNKNOWN";
        }
    }

    bool fpga_field_valid(uint32_t mask, int bit)
    {
        return (mask & (1u << bit)) != 0;
    }

    const char *fpga_video_format_name(uint32_t value)
    {
        switch (value & 0x3u)
        {
        case 0:
            return "YUV422";
        case 1:
            return "RGB";
        case 2:
            return "YUV444";
        case 3:
            return "YUV420";
        default:
            return "UNKNOWN";
        }
    }

    const char *fpga_frame_rate_name(uint32_t value)
    {
        switch (value & 0x0fu)
        {
        case 0x0:
            return "None";
        case 0x2:
            return "23.98";
        case 0x3:
            return "24";
        case 0x4:
            return "47.95";
        case 0x5:
            return "25";
        case 0x6:
            return "29.97";
        case 0x7:
            return "30";
        case 0x8:
            return "48";
        case 0x9:
            return "50";
        case 0xa:
            return "59.94";
        case 0xb:
            return "60";
        default:
            return "--";
        }
    }

    void copy_binary4(char *dst, size_t dstSize, uint32_t value)
    {
        if (!dst || dstSize == 0)
            return;
        const uint32_t code = value & 0x0fu;
        char bits[5] = {};
        for (int i = 0; i < 4; ++i)
            bits[i] = (code & (1u << (3 - i))) ? '1' : '0';
        bits[4] = '\0';
        copy_cstr(dst, dstSize, bits);
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

struct gvfg_handle_t
{
    ~gvfg_handle_t()
    {
        close();
    }

    gvfg_status_t open(int index)
    {
        if (index < 0)
            return GVFG_EINVAL;

        close();

        backend = std::make_unique<gvfg::internal::XdmaCaptureSession>();
        const xdma_status_t stOpen = backend->open_device_index(static_cast<size_t>(index));
        if (stOpen != XDMA_OK)
        {
            emitError(map_status(stOpen), xdma_error_text(stOpen, backend.get()));
            backend.reset();
            return map_status(stOpen);
        }

        currentIndex = index;
        syncBackendEventCallback();
        selectedInput = GDRIVER_INPUT_SDI;
        resetRuntimeCounters();
        const xdma_status_t stInput = backend->set_input(selectedInput);
        if (stInput != XDMA_OK)
        {
            emitError(map_status(stInput), xdma_error_text(stInput, backend.get()));
            close();
            return map_status(stInput);
        }

        querySignal();
        return GVFG_OK;
    }

    gvfg_status_t start()
    {
        if (!backend)
            return GVFG_ESTATE;
        if (running)
            return GVFG_OK;

        const gvfg_status_t cfg = configureStream();
        if (cfg != GVFG_OK)
            return cfg;

        if (previewHwnd && !createRenderPipeline())
            emitError(GVFG_ENOTSUP, "GVFG preview pipeline unavailable");

        resetRuntimeCounters();
        const xdma_status_t st = backend->start_stream();
        if (st != XDMA_OK)
        {
            emitError(map_status(st), xdma_error_text(st, backend.get()));
            return map_status(st);
        }

        running = true;
        captureThread = std::thread([this]()
                                    { captureLoop(); });
        return GVFG_OK;
    }

    gvfg_status_t stop()
    {
        running = false;
        if (captureThread.joinable())
            captureThread.join();
        if (backend)
            backend->stop_stream();
        return GVFG_OK;
    }

    void close()
    {
        stop();
        releaseRenderPipeline();
        if (backend)
        {
            backend->set_event_callback(nullptr, nullptr, 0);
            backend->close();
            backend.reset();
        }
        currentIndex = -1;
    }

    gvfg_status_t setEventCallback(gvfg_on_event_cb callback, void *user, uint32_t mask)
    {
        onEvent = callback;
        eventCallbackUser = user;
        eventMask = mask ? mask : GVFG_EVENT_MASK_DEFAULT;
        syncBackendEventCallback();
        return GVFG_OK;
    }

    gvfg_status_t setPreview(const gvfg_preview_desc_t &desc)
    {
        previewDesc = desc;
        previewHwnd = desc.enable_preview ? desc.hwnd : nullptr;
        if (pipeline)
            pipeline->configurePreview(toGcapPreviewDesc());
        if (previewHwnd && width > 0 && height > 0)
            createRenderPipeline();
        return GVFG_OK;
    }

    gvfg_status_t getSignalStatus(gvfg_signal_status_t &out)
    {
        std::memset(&out, 0, sizeof(out));
        querySignal();
        bool haveSignalSize = false;
        {
            std::lock_guard<std::mutex> lock(stateMutex);
            haveSignalSize = fpgaWidthValid && fpgaHeightValid && fpgaWidthRaw != 0 && fpgaHeightRaw != 0;
            out.width = haveSignalSize ? static_cast<int>(fpgaWidthRaw) : 0;
            out.height = haveSignalSize ? static_cast<int>(fpgaHeightRaw) : 0;
            out.fps = 0.0;
            out.bit_depth = static_cast<int>(bitDepth ? bitDepth : 8);
            copy_cstr(out.pixel_format, sizeof(out.pixel_format), gdriver_pixel_format_name(pixelFormat));
            out.fpga.valid_mask = fpgaValidMask;
            out.fpga.width_valid = fpgaWidthValid ? 1 : 0;
            out.fpga.height_valid = fpgaHeightValid ? 1 : 0;
            out.fpga.width_raw = fpgaWidthRaw;
            out.fpga.height_raw = fpgaHeightRaw;
            out.fpga.video_format_raw = fpgaVideoFormatRaw;
            out.fpga.video_format_valid = fpga_field_valid(fpgaValidMask, 0) ? 1 : 0;
            out.fpga.video_format_code = static_cast<int>(fpgaVideoFormatRaw & 0x3u);
            copy_cstr(out.fpga.video_format, sizeof(out.fpga.video_format), fpga_video_format_name(fpgaVideoFormatRaw));
            out.fpga.frame_rate_raw = fpgaFrameRateRaw;
            out.fpga.frame_rate_valid = fpga_field_valid(fpgaValidMask, 1) ? 1 : 0;
            out.fpga.frame_rate_code = static_cast<int>(fpgaFrameRateRaw & 0x0fu);
            copy_binary4(out.fpga.frame_rate_bits, sizeof(out.fpga.frame_rate_bits), fpgaFrameRateRaw);
            copy_cstr(out.fpga.frame_rate_name, sizeof(out.fpga.frame_rate_name), fpga_frame_rate_name(fpgaFrameRateRaw));
            out.fpga.bit_depth_raw = fpgaBitDepthRaw;
            out.fpga.bit_depth_valid = fpga_field_valid(fpgaValidMask, 2) ? 1 : 0;
            out.fpga.bit_depth = static_cast<int>(fpgaBitDepthRaw);
            out.fpga.status_raw = fpgaStatusRaw;
            out.fpga.status_valid = fpga_field_valid(fpgaValidMask, 3) ? 1 : 0;
            out.fpga.sdi_locked = (fpgaStatusRaw & (1u << 0)) ? 1 : 0;
            out.fpga.sdi_ddr_ok = (fpgaStatusRaw & (1u << 1)) ? 1 : 0;
            out.fpga.hdmi_locked = (fpgaStatusRaw & (1u << 2)) ? 1 : 0;
            out.fpga.hdmi_ddr_ok = (fpgaStatusRaw & (1u << 3)) ? 1 : 0;
        }
        return haveSignalSize ? GVFG_OK : GVFG_ENODEV;
    }

    gvfg_status_t getRuntimeInfo(gvfg_runtime_info_t &out)
    {
        std::memset(&out, 0, sizeof(out));
        getSignalStatus(out.input_signal);
        const uint64_t frames = deliveredFrames.load(std::memory_order_relaxed);
        const bool deliveredValid = running.load(std::memory_order_relaxed) && frames > 0;
        out.delivered_frame.valid = deliveredValid ? 1 : 0;
        if (deliveredValid)
        {
            out.delivered_frame.width = static_cast<int>(deliveredWidth.load(std::memory_order_relaxed));
            out.delivered_frame.height = static_cast<int>(deliveredHeight.load(std::memory_order_relaxed));
            out.delivered_frame.bit_depth = 8;
            copy_cstr(out.delivered_frame.pixel_format, sizeof(out.delivered_frame.pixel_format), "BGRA8");
        }
        out.capture_fps = runtimeFps.load(std::memory_order_relaxed);
        out.delivered_frames = frames;
        return GVFG_OK;
    }

    gvfg_status_t getPreviewInfo(gvfg_preview_info_t &out)
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
        return GVFG_OK;
    }

    void syncBackendEventCallback()
    {
        if (!backend)
            return;
        backend->set_event_callback(onEvent ? &gvfg_handle_t::onBackendEvent : nullptr,
                                    this,
                                    map_event_mask_to_xdma(eventMask));
    }

    static void onBackendEvent(const xdma_event_t *event, void *user)
    {
        auto *self = static_cast<gvfg_handle_t *>(user);
        if (!self || !event)
            return;
        self->emitEvent(*event);
    }

    void emitEvent(const xdma_event_t &event)
    {
        if (!onEvent)
            return;

        gvfg_event_t out{};
        out.type = map_event_type(event.type);
        out.irq_bit = event.irq_bit;
        out.irq_mask = event.irq_mask;
        out.timestamp_ns = event.timestamp_ns;
        onEvent(&out, eventCallbackUser);
    }

    void querySignal()
    {
        if (!backend)
            return;

        xdma_signal_status_t sig{};
        if (backend->get_signal_status(sig) != XDMA_OK)
            return;

        std::lock_guard<std::mutex> lock(stateMutex);
        if (sig.width > 0)
            width = sig.width;
        if (sig.height > 0)
            height = sig.height;
        if (sig.bit_depth > 0)
            bitDepth = sig.bit_depth;
        pixelFormat = sig.pixel_format != GDRIVER_PIXFMT_UNKNOWN ? sig.pixel_format : GDRIVER_PIXFMT_YUY2;
        fpgaValidMask = sig.fpga_valid_mask;
        fpgaWidthValid = sig.fpga_width_valid != 0;
        fpgaHeightValid = sig.fpga_height_valid != 0;
        fpgaWidthRaw = sig.fpga_width_raw;
        fpgaHeightRaw = sig.fpga_height_raw;
        fpgaVideoFormatRaw = sig.fpga_video_format_raw;
        fpgaFrameRateRaw = sig.fpga_frame_rate_raw;
        fpgaBitDepthRaw = sig.fpga_bit_depth_raw;
        fpgaStatusRaw = sig.fpga_status_raw;
    }

    static const char *inputName(gdriver_input_t input)
    {
        switch (input)
        {
        case GDRIVER_INPUT_HDMI:
            return "HDMI";
        case GDRIVER_INPUT_SDI:
            return "SDI";
        default:
            return "unknown";
        }
    }

    gvfg_status_t validateSelectedInputReady()
    {
        const bool statusValid = fpga_field_valid(fpgaValidMask, 3);
        const bool sdiLocked = (fpgaStatusRaw & (1u << 0)) != 0;
        const bool sdiDdrOk = (fpgaStatusRaw & (1u << 1)) != 0;
        const bool hdmiLocked = (fpgaStatusRaw & (1u << 2)) != 0;
        const bool hdmiDdrOk = (fpgaStatusRaw & (1u << 3)) != 0;
        const bool selectedReady = (selectedInput == GDRIVER_INPUT_HDMI) ? (hdmiLocked && hdmiDdrOk)
                                                                         : (sdiLocked && sdiDdrOk);

        if (!statusValid || !selectedReady)
        {
            char msg[256] = {};
            std::snprintf(msg,
                          sizeof(msg),
                          "%s input not ready; FPGA status valid=%d raw=0x%08x sdi_lock=%d sdi_ddr=%d hdmi_lock=%d hdmi_ddr=%d",
                          inputName(selectedInput),
                          statusValid ? 1 : 0,
                          fpgaStatusRaw,
                          sdiLocked ? 1 : 0,
                          sdiDdrOk ? 1 : 0,
                          hdmiLocked ? 1 : 0,
                          hdmiDdrOk ? 1 : 0);
            emitError(GVFG_ENODEV, msg);
            return GVFG_ENODEV;
        }

        if (!fpgaWidthValid || !fpgaHeightValid || fpgaWidthRaw == 0 || fpgaHeightRaw == 0)
        {
            char msg[192] = {};
            std::snprintf(msg,
                          sizeof(msg),
                          "%s input has no valid FPGA resolution; width_valid=%d width=%u height_valid=%d height=%u",
                          inputName(selectedInput),
                          fpgaWidthValid ? 1 : 0,
                          fpgaWidthRaw,
                          fpgaHeightValid ? 1 : 0,
                          fpgaHeightRaw);
            emitError(GVFG_ENODEV, msg);
            return GVFG_ENODEV;
        }

        return GVFG_OK;
    }

    gvfg_status_t configureStream()
    {
        if (!backend)
            return GVFG_ESTATE;

        querySignal();
        const gvfg_status_t inputReady = validateSelectedInputReady();
        if (inputReady != GVFG_OK)
            return inputReady;

        if (width == 0)
            width = 1920;
        if (height == 0)
            height = 1080;

        xdma_stream_desc_t desc{};
        desc.input = selectedInput;
        desc.width = width;
        desc.height = height;
        desc.pixel_format = pixelFormat;
        desc.buffer_count = 1;
        desc.memory_kind = GDRIVER_MEMORY_DRIVER_COPY;

        const xdma_status_t st = backend->configure_stream(desc);
        if (st != XDMA_OK)
        {
            emitError(map_status(st), xdma_error_text(st, backend.get()));
            return map_status(st);
        }
        return GVFG_OK;
    }

    gcap_preview_desc_t toGcapPreviewDesc() const
    {
        gcap_preview_desc_t desc{};
        desc.hwnd = previewHwnd;
        desc.enable_preview = previewHwnd ? 1 : 0;
        desc.use_fp16_pipeline = 1;
        switch (previewDesc.swapchain_bitdepth)
        {
        case GVFG_PREVIEW_BITDEPTH_8BIT:
            desc.swapchain_10bit = GCAP_PREVIEW_BITDEPTH_8BIT;
            break;
        case GVFG_PREVIEW_BITDEPTH_10BIT:
            desc.swapchain_10bit = GCAP_PREVIEW_BITDEPTH_10BIT;
            break;
        case GVFG_PREVIEW_BITDEPTH_AUTO:
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
            xdma_frame_t frame{};
            const xdma_status_t st = backend->wait_frame(1000, frame);
            if (!running)
                break;
            if (st == XDMA_ETIMEOUT)
                continue;
            if (st != XDMA_OK)
            {
                emitError(map_status(st), xdma_error_text(st, backend.get()));
                break;
            }

            updateRuntimeFps(frame.timestamp_ns ? frame.timestamp_ns : now_ns());
            const bool rendered = renderGpuFrame(frame);
            if (rendered)
                emitReadbackFrame(frame);
            else
                emitCpuFallbackFrame(frame);
            backend->release_frame(frame);
        }
    }

    bool renderGpuFrame(const xdma_frame_t &frame)
    {
        if (!pipeline || !previewHwnd || !frame.data)
            return false;

        const int w = static_cast<int>(frame.width);
        const int h = static_cast<int>(frame.height);
        if (w <= 0 || h <= 0)
            return false;

        if (!pipeline->ensure_rt_and_pipeline(w, h) || !pipeline->ensure_preview_swapchain(w, h))
            return false;
        uint32_t fallbackBitDepth = 8;
        {
            std::lock_guard<std::mutex> lock(stateMutex);
            fallbackBitDepth = bitDepth ? bitDepth : 8;
        }
        pipeline->set_source_bit_depth(static_cast<int>(frame.bit_depth ? frame.bit_depth : fallbackBitDepth));

        const auto *base = static_cast<const uint8_t *>(frame.data);
        gcap_pixfmt_t renderFmt = GCAP_FMT_YUY2;
        bool uploaded = false;

        switch (frame.pixel_format)
        {
        case GDRIVER_PIXFMT_YUY2:
        {
            const int stride = frame.plane_stride_bytes[0] ? static_cast<int>(frame.plane_stride_bytes[0]) : w * 2;
            if (stride < w * 2)
                return false;
            renderFmt = GCAP_FMT_YUY2;
            uploaded = pipeline->upload_yuy2_frame(base + frame.plane_offset_bytes[0], stride, w, h);
            break;
        }
        case GDRIVER_PIXFMT_Y210:
        {
            const int stride = frame.plane_stride_bytes[0] ? static_cast<int>(frame.plane_stride_bytes[0]) : w * 4;
            if (stride < w * 4)
                return false;
            renderFmt = GCAP_FMT_Y210;
            uploaded = pipeline->upload_y210_frame(base + frame.plane_offset_bytes[0], stride, w, h);
            break;
        }
        case GDRIVER_PIXFMT_NV12:
        {
            if (frame.plane_count < 2)
                return false;
            const int strideY = frame.plane_stride_bytes[0] ? static_cast<int>(frame.plane_stride_bytes[0]) : w;
            const int strideUV = frame.plane_stride_bytes[1] ? static_cast<int>(frame.plane_stride_bytes[1]) : strideY;
            renderFmt = GCAP_FMT_NV12;
            uploaded = pipeline->upload_nv12_frame(base + frame.plane_offset_bytes[0], strideY,
                                                   base + frame.plane_offset_bytes[1], strideUV, w, h);
            break;
        }
        case GDRIVER_PIXFMT_P010:
        {
            if (frame.plane_count < 2)
                return false;
            const int strideY = frame.plane_stride_bytes[0] ? static_cast<int>(frame.plane_stride_bytes[0]) : w * 2;
            const int strideUV = frame.plane_stride_bytes[1] ? static_cast<int>(frame.plane_stride_bytes[1]) : strideY;
            renderFmt = GCAP_FMT_P010;
            uploaded = pipeline->upload_p010_frame(base + frame.plane_offset_bytes[0], strideY,
                                                   base + frame.plane_offset_bytes[1], strideUV, w, h);
            break;
        }
        default:
            return false;
        }

        if (!uploaded ||
            !pipeline->render_uploaded_yuv_to_fp16(renderFmt, w, h) ||
            !pipeline->copy_fp16_to_scene())
            return false;

        bool ok = true;
        if (!pipeline->preview_swapchain_10bit())
            ok = pipeline->blit_fp16_to_rgba8(w, h);
        if (ok)
            pipeline->present_preview(w, h);
        return ok;
    }

    void emitReadbackFrame(const xdma_frame_t &frame)
    {
        if (!onFrame || !pipeline || !ctx)
            return;
        if (!shouldEmitFrameCallback(frame.frame_id))
            return;

        const int w = static_cast<int>(frame.width);
        const int h = static_cast<int>(frame.height);
        if (!pipeline->blit_fp16_to_rgba8(w, h))
            return;

        gcap_frame_t readback{};
        if (pipeline->readback_to_frame(w, h, frame.timestamp_ns ? frame.timestamp_ns : now_ns(), frame.frame_id, &readback))
        {
            gvfg_frame_t out{};
            out.data = readback.data[0];
            out.stride = readback.stride[0];
            out.width = readback.width;
            out.height = readback.height;
            out.pts_ns = readback.pts_ns;
            out.frame_id = readback.frame_id;
            onFrame(&out, callbackUser);
            noteDeliveredFrame(out.width, out.height);
            ctx->Unmap(pipeline->rt_stage_.Get(), 0);
        }
    }

    void emitCpuFallbackFrame(const xdma_frame_t &frame)
    {
        if (frame.pixel_format != GDRIVER_PIXFMT_YUY2)
        {
            if (!fallbackLogged)
            {
                emitError(GVFG_ENOTSUP, "GVFG preview currently supports YUV422/YUV420 GPU paths only; RGB/YUV444 input is not rendered");
                fallbackLogged = true;
            }
            return;
        }
        if (!onFrame || !frame.data)
            return;
        if (!shouldEmitFrameCallback(frame.frame_id))
            return;

        const int w = static_cast<int>(frame.width);
        const int h = static_cast<int>(frame.height);
        const int srcStride = frame.plane_stride_bytes[0] ? static_cast<int>(frame.plane_stride_bytes[0]) : w * 2;
        if (w <= 0 || h <= 0 || srcStride < w * 2)
            return;

        if (!fallbackLogged)
        {
            emitError(GVFG_ENOTSUP, "GPU preview/readback unavailable; using CPU YUY2 preview fallback");
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

        gvfg_frame_t out{};
        out.data = fallbackBgra.data();
        out.stride = w * 4;
        out.width = w;
        out.height = h;
        out.pts_ns = frame.timestamp_ns ? frame.timestamp_ns : now_ns();
        out.frame_id = frame.frame_id;
        onFrame(&out, callbackUser);
        noteDeliveredFrame(out.width, out.height);
    }

    bool shouldEmitFrameCallback(uint64_t frameId) const
    {
        const uint32_t interval = callbackFrameInterval.load(std::memory_order_relaxed);
        if (interval <= 1)
            return true;
        return (frameId % interval) == 1;
    }

    void emitError(gvfg_status_t code, const char *msg)
    {
        if (onError)
            onError(code, msg ? msg : "", callbackUser);
    }

    void updateRuntimeFps(uint64_t ptsNs)
    {
        const uint64_t prevPtsNs = lastPtsNs.exchange(ptsNs, std::memory_order_relaxed);
        if (prevPtsNs != 0 && ptsNs > prevPtsNs)
        {
            const double fps = 1e9 / static_cast<double>(ptsNs - prevPtsNs);
            if (fps > 0.0 && fps < 1000.0)
            {
                const double current = runtimeFps.load(std::memory_order_relaxed);
                runtimeFps.store((current <= 0.0) ? fps : current * 0.9 + fps * 0.1,
                                 std::memory_order_relaxed);
            }
        }
    }

    void resetRuntimeCounters()
    {
        lastPtsNs.store(0, std::memory_order_relaxed);
        deliveredFrames.store(0, std::memory_order_relaxed);
        deliveredWidth.store(0, std::memory_order_relaxed);
        deliveredHeight.store(0, std::memory_order_relaxed);
        runtimeFps.store(0.0, std::memory_order_relaxed);
    }

    void noteDeliveredFrame(int frameWidth, int frameHeight)
    {
        deliveredWidth.store(frameWidth > 0 ? static_cast<uint32_t>(frameWidth) : 0, std::memory_order_relaxed);
        deliveredHeight.store(frameHeight > 0 ? static_cast<uint32_t>(frameHeight) : 0, std::memory_order_relaxed);
        deliveredFrames.fetch_add(1, std::memory_order_relaxed);
    }

    std::unique_ptr<gvfg::internal::XdmaCaptureSession> backend;
    int currentIndex = -1;
    gdriver_input_t selectedInput = GDRIVER_INPUT_SDI;
    gvfg_preview_desc_t previewDesc{};
    void *previewHwnd = nullptr;

    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t bitDepth = 8;
    gdriver_pixel_format_t pixelFormat = GDRIVER_PIXFMT_YUY2;
    mutable std::mutex stateMutex;
    uint32_t fpgaValidMask = 0;
    bool fpgaWidthValid = false;
    bool fpgaHeightValid = false;
    uint32_t fpgaWidthRaw = 0;
    uint32_t fpgaHeightRaw = 0;
    uint32_t fpgaVideoFormatRaw = 0;
    uint32_t fpgaFrameRateRaw = 0;
    uint32_t fpgaBitDepthRaw = 0;
    uint32_t fpgaStatusRaw = 0;
    std::atomic<uint64_t> lastPtsNs{0};
    std::atomic<uint64_t> deliveredFrames{0};
    std::atomic<uint32_t> deliveredWidth{0};
    std::atomic<uint32_t> deliveredHeight{0};
    std::atomic<double> runtimeFps{0.0};

    gvfg_on_frame_cb onFrame = nullptr;
    gvfg_on_error_cb onError = nullptr;
    void *callbackUser = nullptr;
    std::atomic<uint32_t> callbackFrameInterval{1};
    gvfg_on_event_cb onEvent = nullptr;
    void *eventCallbackUser = nullptr;
    uint32_t eventMask = GVFG_EVENT_MASK_DEFAULT;
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
    int gvfg_enumerate_devices(gvfg_device_info_t *out_devices, int max_devices)
    {
        const std::vector<gvfg::internal::XdmaDevice> devices = gvfg::internal::enumerate_xdma_devices();
        const int n = static_cast<int>(devices.size());
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
                      "GVFG Capture");
            copy_wide_to_utf8(devices[static_cast<size_t>(i)].friendly_name,
                              out_devices[i].name,
                              sizeof(out_devices[i].name));
            if (!out_devices[i].name[0])
                copy_cstr(out_devices[i].name, sizeof(out_devices[i].name), "GVFG Capture");
        }
        return written;
    }

    gvfg_status_t gvfg_create(gvfg_handle *out_handle)
    {
        if (!out_handle)
            return GVFG_EINVAL;
        auto h = std::make_unique<gvfg_handle_t>();
        *out_handle = h.release();
        return GVFG_OK;
    }

    gvfg_status_t gvfg_destroy(gvfg_handle handle)
    {
        delete handle;
        return GVFG_OK;
    }

    gvfg_status_t gvfg_set_callbacks(gvfg_handle handle,
                                     gvfg_on_frame_cb on_frame,
                                     gvfg_on_error_cb on_error,
                                     void *user)
    {
        if (!handle)
            return GVFG_EINVAL;
        handle->onFrame = on_frame;
        handle->onError = on_error;
        handle->callbackUser = user;
        return GVFG_OK;
    }

    gvfg_status_t gvfg_set_frame_callback_interval(gvfg_handle handle, uint32_t frame_interval)
    {
        if (!handle)
            return GVFG_EINVAL;
        handle->callbackFrameInterval.store(frame_interval <= 1 ? 1u : frame_interval,
                                            std::memory_order_relaxed);
        return GVFG_OK;
    }

    gvfg_status_t gvfg_set_event_callback(gvfg_handle handle,
                                          gvfg_on_event_cb on_event,
                                          void *user,
                                          uint32_t event_mask)
    {
        if (!handle)
            return GVFG_EINVAL;
        return handle->setEventCallback(on_event, user, event_mask);
    }

    gvfg_status_t gvfg_set_preview(gvfg_handle handle, const gvfg_preview_desc_t *desc)
    {
        if (!handle || !desc)
            return GVFG_EINVAL;
        return handle->setPreview(*desc);
    }

    gvfg_status_t gvfg_open(gvfg_handle handle, int device_index)
    {
        if (!handle)
            return GVFG_EINVAL;
        return handle->open(device_index);
    }

    gvfg_status_t gvfg_start(gvfg_handle handle)
    {
        if (!handle)
            return GVFG_EINVAL;
        return handle->start();
    }

    gvfg_status_t gvfg_stop(gvfg_handle handle)
    {
        if (!handle)
            return GVFG_EINVAL;
        return handle->stop();
    }

    gvfg_status_t gvfg_get_signal_status(gvfg_handle handle, gvfg_signal_status_t *out_status)
    {
        if (!handle || !out_status)
            return GVFG_EINVAL;
        return handle->getSignalStatus(*out_status);
    }

    gvfg_status_t gvfg_get_runtime_info(gvfg_handle handle, gvfg_runtime_info_t *out_info)
    {
        if (!handle || !out_info)
            return GVFG_EINVAL;
        return handle->getRuntimeInfo(*out_info);
    }

    gvfg_status_t gvfg_get_preview_info(gvfg_handle handle, gvfg_preview_info_t *out_info)
    {
        if (!handle || !out_info)
            return GVFG_EINVAL;
        return handle->getPreviewInfo(*out_info);
    }

    const char *gvfg_strerror(gvfg_status_t status)
    {
        switch (status)
        {
        case GVFG_OK:
            return "OK";
        case GVFG_EINVAL:
            return "Invalid argument";
        case GVFG_ENODEV:
            return "No GVFG device";
        case GVFG_ESTATE:
            return "Invalid state";
        case GVFG_EIO:
            return "I/O error";
        case GVFG_ENOTSUP:
            return "Not supported";
        case GVFG_ETIMEOUT:
            return "Timeout";
        default:
            return "Unknown";
        }
    }
}
