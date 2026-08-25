// src/core/c_api.cpp
#include "capture_manager.h"
#include "frame_converter.h"
#include "logging.h"
#ifndef GCAPTURE_BUILD
#error not exporting
#endif
#include "gcapture.h"
#include <memory>
#include <vector>
#include <cstring>
#include <cstdio>
#include <algorithm>
#include <string>
#include <fstream>
#include <filesystem>
#include "../audio/audio_manager.h"
#include "gcap_audio.h"
#include "../providers/dshow_signal_probe.h"
#include "../providers/winmf_provider.h"

#ifndef GCAPTURE_SDK_VERSION
#define GCAPTURE_SDK_VERSION "1.0.0"
#endif

#ifdef _WIN32
#include <windows.h>
#include <wrl/client.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <wincodec.h>
#pragma comment(lib, "ole32.lib")
using Microsoft::WRL::ComPtr;

static std::string w2utf8(const wchar_t *ws)
{
    if (!ws)
        return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, ws, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 0)
        return {};
    std::string out(static_cast<size_t>(len), '\0');
    if (WideCharToMultiByte(CP_UTF8, 0, ws, -1, out.data(), len, nullptr, nullptr) <= 0)
        return {};
    out.pop_back();
    return out;
}

static std::wstring utf8_to_wide_path(const char *s)
{
    if (!s || !*s)
        return {};
    const int len = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    if (len <= 0)
        return {};
    std::wstring out(static_cast<size_t>(len), L'\0');
    if (MultiByteToWideChar(CP_UTF8, 0, s, -1, out.data(), len) <= 0)
        return {};
    out.pop_back();
    return out;
}

static std::wstring wide_append_suffix(const char *base_utf8, const wchar_t *suffix)
{
    std::wstring out = utf8_to_wide_path(base_utf8);
    if (!out.empty() && suffix)
        out += suffix;
    return out;
}

static bool wide_file_exists(const std::wstring &path)
{
    if (path.empty())
        return false;
    const DWORD attr = GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

static bool read_file_exact(const std::wstring &path, std::vector<uint8_t> &out, size_t expected_bytes)
{
    out.clear();
    if (path.empty() || expected_bytes == 0)
        return false;
    std::ifstream ifs(std::filesystem::path(path), std::ios::binary);
    if (!ifs)
        return false;
    out.resize(expected_bytes);
    ifs.read(reinterpret_cast<char *>(out.data()), static_cast<std::streamsize>(expected_bytes));
    return static_cast<size_t>(ifs.gcount()) == expected_bytes;
}

static bool write_bgra8_png_wic(const std::wstring &path, int width, int height, const std::vector<uint8_t> &bgra)
{
    if (path.empty() || width <= 0 || height <= 0)
        return false;
    const size_t expected = static_cast<size_t>(width) * static_cast<size_t>(height) * 4u;
    if (bgra.size() < expected)
        return false;

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool needUninit = SUCCEEDED(hr);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE)
        return false;

    IWICImagingFactory *factory = nullptr;
    IWICBitmapEncoder *encoder = nullptr;
    IWICStream *stream = nullptr;
    IWICBitmapFrameEncode *frame = nullptr;
    IPropertyBag2 *bag = nullptr;
    bool ok = false;

    do
    {
        hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
        if (FAILED(hr) || !factory)
            break;
        hr = factory->CreateStream(&stream);
        if (FAILED(hr) || !stream)
            break;
        hr = stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE);
        if (FAILED(hr))
            break;
        hr = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
        if (FAILED(hr) || !encoder)
            break;
        hr = encoder->Initialize(stream, WICBitmapEncoderNoCache);
        if (FAILED(hr))
            break;
        hr = encoder->CreateNewFrame(&frame, &bag);
        if (FAILED(hr) || !frame)
            break;
        hr = frame->Initialize(bag);
        if (FAILED(hr))
            break;
        hr = frame->SetSize(static_cast<UINT>(width), static_cast<UINT>(height));
        if (FAILED(hr))
            break;
        WICPixelFormatGUID pf = GUID_WICPixelFormat32bppBGRA;
        hr = frame->SetPixelFormat(&pf);
        if (FAILED(hr) || !IsEqualGUID(pf, GUID_WICPixelFormat32bppBGRA))
            break;
        const UINT stride = static_cast<UINT>(width * 4);
        const UINT imageSize = static_cast<UINT>(expected);
        hr = frame->WritePixels(static_cast<UINT>(height), stride, imageSize, const_cast<BYTE *>(bgra.data()));
        if (FAILED(hr))
            break;
        hr = frame->Commit();
        if (FAILED(hr))
            break;
        hr = encoder->Commit();
        if (FAILED(hr))
            break;
        ok = true;
    } while (false);

    if (bag)
        bag->Release();
    if (frame)
        frame->Release();
    if (encoder)
        encoder->Release();
    if (stream)
        stream->Release();
    if (factory)
        factory->Release();
    if (needUninit)
        CoUninitialize();
    return ok;
}

static bool create_png_from_abgr2101010_raw(const std::wstring &raw_path, const std::wstring &png_path, int width, int height)
{
    const size_t pixelCount = static_cast<size_t>(width) * static_cast<size_t>(height);
    std::vector<uint8_t> raw;
    if (!read_file_exact(raw_path, raw, pixelCount * sizeof(uint32_t)))
        return false;

    std::vector<uint8_t> bgra(pixelCount * 4u);
    for (size_t i = 0; i < pixelCount; ++i)
    {
        const size_t ri = i * 4u;
        const uint32_t p = static_cast<uint32_t>(raw[ri + 0u]) |
                           (static_cast<uint32_t>(raw[ri + 1u]) << 8u) |
                           (static_cast<uint32_t>(raw[ri + 2u]) << 16u) |
                           (static_cast<uint32_t>(raw[ri + 3u]) << 24u);
        const uint32_t r10 = (p >> 0u) & 0x3FFu;
        const uint32_t g10 = (p >> 10u) & 0x3FFu;
        const uint32_t b10 = (p >> 20u) & 0x3FFu;
        const size_t bi = i * 4u;
        bgra[bi + 0u] = static_cast<uint8_t>((b10 * 255u + 511u) / 1023u);
        bgra[bi + 1u] = static_cast<uint8_t>((g10 * 255u + 511u) / 1023u);
        bgra[bi + 2u] = static_cast<uint8_t>((r10 * 255u + 511u) / 1023u);
        bgra[bi + 3u] = 255u;
    }
    return write_bgra8_png_wic(png_path, width, height, bgra);
}

static bool create_png_from_bgra8_raw(const std::wstring &raw_path, const std::wstring &png_path, int width, int height)
{
    std::vector<uint8_t> bgra;
    if (!read_file_exact(raw_path, bgra, static_cast<size_t>(width) * static_cast<size_t>(height) * 4u))
        return false;
    return write_bgra8_png_wic(png_path, width, height, bgra);
}

#endif

extern "C"
{
    // 簡單的 handle 物件，內含一個 CaptureManager
    struct gcap_handle_t
    {
        CaptureManager mgr;
    };

    static void copy_cstr(char *dst, size_t dstSize, const char *src)
    {
        if (!dst || dstSize == 0)
            return;
        if (!src)
            src = "";
        std::snprintf(dst, dstSize, "%s", src);
    }

    static void copy_path(char *dst, size_t dstSize, const char *base, const char *suffix)
    {
        if (!dst || dstSize == 0)
            return;
        if (!base)
            base = "";
        if (!suffix)
            suffix = "";
        std::snprintf(dst, dstSize, "%s%s", base, suffix);
    }

    static int format_rank_for_backend(int backend, gcap_pixfmt_t fmt)
    {
        if (backend == GCAP_BACKEND_DSHOW)
        {
            switch (fmt)
            {
            case GCAP_FMT_Y210: return 500;
            case GCAP_FMT_YUY2: return 400;
            case GCAP_FMT_NV12: return 300;
            case GCAP_FMT_ARGB: return 200;
            case GCAP_FMT_P010: return 100;
            default: return 0;
            }
        }
        switch (fmt)
        {
        case GCAP_FMT_P010: return 500;
        case GCAP_FMT_Y210: return 450;
        case GCAP_FMT_YUY2: return 400;
        case GCAP_FMT_NV12: return 300;
        case GCAP_FMT_ARGB: return 200;
        default: return 0;
        }
    }

    const char *gcap_pixfmt_name(gcap_pixfmt_t fmt)
    {
        switch (fmt)
        {
        case GCAP_FMT_NV12: return "NV12";
        case GCAP_FMT_YUY2: return "YUY2";
        case GCAP_FMT_ARGB: return "ARGB/RGB32";
        case GCAP_FMT_P010: return "P010";
        case GCAP_FMT_Y210: return "Y210";
        case GCAP_FMT_V210: return "V210";
        case GCAP_FMT_R210: return "R210";
        default: return "UNKNOWN";
        }
    }

    const char *gcap_version_string(void)
    {
        return GCAPTURE_SDK_VERSION;
    }

    int gcap_pixfmt_bit_depth(gcap_pixfmt_t fmt)
    {
        switch (fmt)
        {
        case GCAP_FMT_P010:
        case GCAP_FMT_Y210:
        case GCAP_FMT_V210:
        case GCAP_FMT_R210:
            return 10;
        case GCAP_FMT_NV12:
        case GCAP_FMT_YUY2:
        case GCAP_FMT_ARGB:
            return 8;
        default:
            return 0;
        }
    }

    int gcap_pixfmt_is_10bit(gcap_pixfmt_t fmt)
    {
        return gcap_pixfmt_bit_depth(fmt) == 10 ? 1 : 0;
    }

    int gcap_pixfmt_is_yuv(gcap_pixfmt_t fmt)
    {
        switch (fmt)
        {
        case GCAP_FMT_NV12:
        case GCAP_FMT_YUY2:
        case GCAP_FMT_P010:
        case GCAP_FMT_Y210:
        case GCAP_FMT_V210:
            return 1;
        default:
            return 0;
        }
    }

    int gcap_pixfmt_default_stride(int width, gcap_pixfmt_t fmt)
    {
        if (width <= 0)
            return 0;
        switch (fmt)
        {
        case GCAP_FMT_NV12: return width;
        case GCAP_FMT_P010: return width * 2;
        case GCAP_FMT_YUY2: return width * 2;
        case GCAP_FMT_Y210: return width * 4;
        case GCAP_FMT_ARGB: return width * 4;
        case GCAP_FMT_R210: return width * 4;
        case GCAP_FMT_V210: return ((width + 5) / 6) * 16;
        default: return 0;
        }
    }

    const char *gcap_backend_name(int backend)
    {
        switch (backend)
        {
        case GCAP_BACKEND_WINMF_CPU: return "WinMF CPU";
        case GCAP_BACKEND_WINMF_GPU: return "WinMF GPU";
        case GCAP_BACKEND_DSHOW: return "DirectShow";
        case GCAP_BACKEND_AUTO: return "Auto";
        default: return "Unknown";
        }
    }

    const char *gcap_source_kind_name(int source_kind)
    {
        switch (source_kind)
        {
        case GCAP_SOURCE_WINMF_GPU: return "WinMF GPU";
        case GCAP_SOURCE_WINMF_CPU: return "WinMF CPU";
        case GCAP_SOURCE_DSHOW_RAWSINK: return "DShow RawSink";
        case GCAP_SOURCE_DSHOW_RENDERER: return "DShow Renderer (legacy)";
        case GCAP_SOURCE_UNKNOWN:
        default: return "Unknown";
        }
    }

    int gcap_recording_uses_hevc_main10(gcap_pixfmt_t fmt)
    {
        return (fmt == GCAP_FMT_P010 || fmt == GCAP_FMT_Y210) ? 1 : 0;
    }

    const char *gcap_recording_mode_name(int backend)
    {
        switch (backend)
        {
        case GCAP_BACKEND_DSHOW: return "DShow + FFmpeg MP4";
        case GCAP_BACKEND_WINMF_CPU:
        case GCAP_BACKEND_WINMF_GPU: return "Media Foundation Sink Writer";
        case GCAP_BACKEND_AUTO: return "Auto Recorder";
        default: return "Recorder";
        }
    }

    // 錯誤字串
    const char *gcap_strerror(gcap_status_t s)
    {
        switch (s)
        {
        case GCAP_OK:
            return "OK";
        case GCAP_EINVAL:
            return "Invalid argument";
        case GCAP_ENODEV:
            return "No such device";
        case GCAP_ESTATE:
            return "Invalid state";
        case GCAP_EIO:
            return "I/O error";
        case GCAP_ENOTSUP:
            return "Not supported";
        default:
            return "Unknown";
        }
    }

    // 只為了列舉裝置，不需要長壽命 handle
    gcap_status_t gcap_enumerate(gcap_device_info_t *out, int max, int *count)
    {
        if (!out || max <= 0)
            return GCAP_EINVAL;
        CaptureManager tmp;
        return tmp.enumerate(out, max, count);
    }

    gcap_status_t gcap_create(gcap_handle *out)
    {
        if (!out)
            return GCAP_EINVAL;
        auto h = std::make_unique<gcap_handle_t>();
        *out = h.release();
        return GCAP_OK;
    }

    gcap_status_t gcap_open(int device_index, gcap_handle *out)
    {
        if (!out)
            return GCAP_EINVAL;
        auto h = std::make_unique<gcap_handle_t>();
        gcap_status_t st = h->mgr.open(device_index);
        if (st != GCAP_OK)
            return st;
        *out = h.release();
        return GCAP_OK;
    }

    gcap_status_t gcap_open2(gcap_handle h, int device_index)
    {
        if (!h)
            return GCAP_EINVAL;
        return h->mgr.open(device_index);
    }

    gcap_status_t gcap_set_profile(gcap_handle h, const gcap_profile_t *p)
    {
        if (!h || !p)
            return GCAP_EINVAL;
        return h->mgr.setProfile(*p);
    }

    gcap_status_t gcap_set_buffers(gcap_handle h, int count, size_t bytes_hint)
    {
        if (!h)
            return GCAP_EINVAL;
        return h->mgr.setBuffers(count, bytes_hint);
    }

    gcap_status_t gcap_set_callbacks(gcap_handle h,
                                     gcap_on_video_cb vcb,
                                     gcap_on_error_cb ecb,
                                     void *user)
    {
        if (!h)
            return GCAP_EINVAL;
        return h->mgr.setCallbacks(vcb, ecb, user);
    }

    gcap_status_t gcap_set_frame_packet_callback(gcap_handle h,
                                                 gcap_on_frame_packet_cb cb,
                                                 void *user)
    {
        if (!h)
            return GCAP_EINVAL;
        {
            char buf[256];
            gcap::log_printf(GCAP_LOG_DEBUG, "[gcapture] set_frame_packet_callback h=%p cb=%p user=%p\n",
                         h, reinterpret_cast<void *>(cb), user);
        }
        return h->mgr.setFramePacketCallback(cb, user);
    }

    int gcap_enum_video_caps(int device_index, gcap_video_cap_t *out_caps, int max_caps)
    {
#ifdef _WIN32
        return dshow_enum_video_caps_by_index(device_index, out_caps, max_caps);
#else
        (void)device_index;
        (void)out_caps;
        (void)max_caps;
        return 0;
#endif
    }

    int gcap_enum_video_caps_ex(int backend, int device_index, gcap_video_cap_t *out_caps, int max_caps)
    {
#ifdef _WIN32
        if (backend == GCAP_BACKEND_DSHOW)
            return dshow_enum_video_caps_by_index(device_index, out_caps, max_caps);
        // Current WinMF helper exposes unique pixel formats only. Keep this API backend-aware and conservative.
        if (backend == GCAP_BACKEND_WINMF_CPU || backend == GCAP_BACKEND_WINMF_GPU || backend == GCAP_BACKEND_AUTO)
            return 0;
        return 0;
#else
        (void)backend; (void)device_index; (void)out_caps; (void)max_caps;
        return 0;
#endif
    }

    gcap_status_t gcap_get_recommended_profile(int backend, int device_index, gcap_profile_t *out_profile)
    {
        if (!out_profile)
            return GCAP_EINVAL;
        std::memset(out_profile, 0, sizeof(*out_profile));
        out_profile->mode = GCAP_PROFILE_DEVICE_DEFAULT;

#ifdef _WIN32
        if (backend == GCAP_BACKEND_DSHOW)
        {
            const int count = dshow_enum_video_caps_by_index(device_index, nullptr, 0);
            if (count <= 0)
                return GCAP_ENOTSUP;
            std::vector<gcap_video_cap_t> caps(static_cast<size_t>(count));
            const int n = dshow_enum_video_caps_by_index(device_index, caps.data(), count);
            if (n <= 0)
                return GCAP_ENOTSUP;
            int best = -1;
            long long bestScore = -1;
            for (int i = 0; i < n; ++i)
            {
                const auto &c = caps[static_cast<size_t>(i)];
                const int fps = c.fps_den > 0 ? (c.fps_num / c.fps_den) : c.fps_num;
                const long long score = static_cast<long long>(format_rank_for_backend(backend, c.pixfmt)) * 100000000LL
                                      + static_cast<long long>(c.width) * static_cast<long long>(c.height)
                                      + fps;
                if (score > bestScore)
                {
                    bestScore = score;
                    best = i;
                }
            }
            if (best < 0)
                return GCAP_ENOTSUP;
            const auto &c = caps[static_cast<size_t>(best)];
            out_profile->width = c.width;
            out_profile->height = c.height;
            out_profile->fps_num = c.fps_num;
            out_profile->fps_den = c.fps_den > 0 ? c.fps_den : 1;
            out_profile->format = c.pixfmt;
            out_profile->mode = GCAP_PROFILE_CUSTOM;
            return GCAP_OK;
        }

        if (backend == GCAP_BACKEND_WINMF_CPU || backend == GCAP_BACKEND_WINMF_GPU || backend == GCAP_BACKEND_AUTO)
        {
            gcap_pixfmt_t formats[16] = {};
            const int n = winmf_enum_supported_pixel_formats_by_index(device_index, formats, 16);
            if (n <= 0)
                return GCAP_ENOTSUP;
            int best = -1;
            int bestScore = -1;
            for (int i = 0; i < n; ++i)
            {
                const int score = format_rank_for_backend(backend, formats[i]);
                if (score > bestScore)
                {
                    bestScore = score;
                    best = i;
                }
            }
            if (best < 0)
                return GCAP_ENOTSUP;
            out_profile->format = formats[best];
            out_profile->mode = GCAP_PROFILE_DEVICE_DEFAULT;
            return GCAP_OK;
        }
#else
        (void)backend; (void)device_index;
#endif
        return GCAP_ENOTSUP;
    }

    int gcap_enum_supported_pixel_formats(int backend, int device_index, gcap_pixfmt_t *out_formats, int max_formats)
    {
#ifdef _WIN32
        if (backend == GCAP_BACKEND_DSHOW)
        {
            const int capCount = dshow_enum_video_caps_by_index(device_index, nullptr, 0);
            if (capCount <= 0)
                return 0;
            std::vector<gcap_video_cap_t> caps(static_cast<size_t>(capCount));
            const int written = dshow_enum_video_caps_by_index(device_index, caps.data(), static_cast<int>(caps.size()));
            std::vector<gcap_pixfmt_t> uniq;
            auto push_unique = [&](gcap_pixfmt_t fmt)
            {
                if (fmt != GCAP_FMT_NV12 && fmt != GCAP_FMT_YUY2 && fmt != GCAP_FMT_Y210 && fmt != GCAP_FMT_V210 && fmt != GCAP_FMT_P010 && fmt != GCAP_FMT_ARGB)
                    return;
                for (auto e : uniq)
                    if (e == fmt)
                        return;
                uniq.push_back(fmt);
            };
            for (int i = 0; i < written; ++i)
                push_unique(caps[static_cast<size_t>(i)].pixfmt);
            if (!out_formats || max_formats <= 0)
                return static_cast<int>(uniq.size());
            const int n = (std::min)(max_formats, static_cast<int>(uniq.size()));
            for (int i = 0; i < n; ++i)
                out_formats[i] = uniq[static_cast<size_t>(i)];
            return n;
        }
        if (backend == GCAP_BACKEND_WINMF_CPU || backend == GCAP_BACKEND_WINMF_GPU || backend == GCAP_BACKEND_AUTO)
            return winmf_enum_supported_pixel_formats_by_index(device_index, out_formats, max_formats);
        return 0;
#else
        (void)backend;
        (void)device_index;
        (void)out_formats;
        (void)max_formats;
        return 0;
#endif
    }

    int gcap_enum_property_pages(int device_index, gcap_property_page_t *out_pages, int max_pages)
    {
#ifdef _WIN32
        return dshow_enum_property_pages_by_index(device_index, out_pages, max_pages);
#else
        (void)device_index;
        (void)out_pages;
        (void)max_pages;
        return 0;
#endif
    }

    int gcap_open_vendor_property_page(int device_index)
    {
        return dshow_open_vendor_property_page_by_index(device_index) ? 1 : 0;
    }

    int gcap_open_named_property_page(int device_index, const char *page_name_utf8, int capture_pin)
    {
        if (!page_name_utf8 || !*page_name_utf8)
            return 0;
        wchar_t wname[256] = {};
        MultiByteToWideChar(CP_UTF8, 0, page_name_utf8, -1, wname, 256);
        return dshow_open_named_property_page_by_index(device_index, wname, capture_pin != 0) ? 1 : 0;
    }

    gcap_status_t gcap_start(gcap_handle h)
    {
        if (!h)
            return GCAP_EINVAL;
        return h->mgr.start();
    }

    gcap_status_t gcap_start_recording(gcap_handle h, const char *path_utf8)
    {
        if (!h)
            return GCAP_EINVAL;
        return h->mgr.startRecording(path_utf8);
    }

    gcap_status_t gcap_stop_recording(gcap_handle h)
    {
        if (!h)
            return GCAP_EINVAL;
        return h->mgr.stopRecording();
    }

    gcap_status_t gcap_get_recording_info(gcap_handle h, gcap_recording_info_t *out)
    {
        if (!h || !out)
            return GCAP_EINVAL;

        std::memset(out, 0, sizeof(*out));

        gcap_runtime_info_t rt{};
        const gcap_status_t st = h->mgr.getRuntimeInfo(rt);
        if (st != GCAP_OK)
            return st;

        const int backend = (rt.active_backend >= 0) ? rt.active_backend : h->mgr.getActiveBackendInt();
        gcap_pixfmt_t fmt = rt.negotiated.pixfmt;
        if (gcap_pixfmt_bit_depth(fmt) == 0)
            fmt = rt.signal.pixfmt;

        const char *fmtName = gcap_pixfmt_name(fmt);
        const int inputDepth = gcap_pixfmt_bit_depth(fmt);
        const int hevcMain10 = gcap_recording_uses_hevc_main10(fmt);

        copy_cstr(out->mode_name, sizeof(out->mode_name), gcap_recording_mode_name(backend));
        copy_cstr(out->input_format, sizeof(out->input_format), fmtName);
        out->input_bit_depth = inputDepth;
        out->output_fps_num = rt.negotiated.fps_num > 0 ? rt.negotiated.fps_num : rt.signal.fps_num;
        out->output_fps_den = rt.negotiated.fps_den > 0 ? rt.negotiated.fps_den : rt.signal.fps_den;
        if (out->output_fps_num <= 0) out->output_fps_num = 30;
        if (out->output_fps_den <= 0) out->output_fps_den = 1;

        if (backend == GCAP_BACKEND_DSHOW)
        {
            out->video_only = 1;
            if (fmt == GCAP_FMT_P010)
            {
                copy_cstr(out->encoder_name, sizeof(out->encoder_name),
                          "FFmpeg HEVC / H.265 via Media Foundation (input P010 10-bit, LGPL-friendly video-only path)");
                copy_cstr(out->output_format, sizeof(out->output_format), "HEVC");
                out->output_bit_depth = 8;
            }
            else if (fmt == GCAP_FMT_Y210)
            {
                copy_cstr(out->encoder_name, sizeof(out->encoder_name),
                          "FFmpeg HEVC / H.265 via Media Foundation (input Y210 10-bit 4:2:2, LGPL-friendly video-only path)");
                copy_cstr(out->output_format, sizeof(out->output_format), "HEVC");
                out->output_bit_depth = 8;
            }
            else
            {
                char label[160] = {};
                std::snprintf(label, sizeof(label),
                              "FFmpeg H.264 / AVC via Media Foundation (input %s, LGPL-friendly video-only path)", fmtName);
                copy_cstr(out->encoder_name, sizeof(out->encoder_name), label);
                copy_cstr(out->output_format, sizeof(out->output_format), "H.264");
                out->output_bit_depth = 8;
            }
            // Current DShow FFmpeg recorder writes fixed output cadence.
            out->output_fps_num = 30;
            out->output_fps_den = 1;
            return GCAP_OK;
        }

        out->video_only = 0;
        if (hevcMain10)
        {
            char label[160] = {};
            std::snprintf(label, sizeof(label),
                          "Media Foundation HEVC / H.265 Encoder (Sink Writer, input %s / 10-bit)", fmtName);
            copy_cstr(out->encoder_name, sizeof(out->encoder_name), label);
            copy_cstr(out->output_format, sizeof(out->output_format), "HEVC Main10");
            out->output_bit_depth = 10;
        }
        else
        {
            char label[160] = {};
            std::snprintf(label, sizeof(label),
                          "Media Foundation H.264 / AVC Encoder (Sink Writer, input %s / 8-bit)", fmtName);
            copy_cstr(out->encoder_name, sizeof(out->encoder_name), label);
            copy_cstr(out->output_format, sizeof(out->output_format), "H.264 8-bit");
            out->output_bit_depth = 8;
        }
        return GCAP_OK;
    }

    GCAP_API gcap_status_t gcap_get_recording_stats(gcap_handle h, gcap_recording_stats_t *out)
    {
        if (!h || !out)
            return GCAP_EINVAL;
        std::memset(out, 0, sizeof(*out));
        const gcap_status_t st = h->mgr.getRecordingStats(*out);
        if (st != GCAP_OK)
            return st;

        if (out->encoder_name[0] == 0)
        {
            gcap_recording_info_t info{};
            if (gcap_get_recording_info(h, &info) == GCAP_OK)
            {
                copy_cstr(out->encoder_name, sizeof(out->encoder_name), info.encoder_name);
                out->output_bit_depth = info.output_bit_depth;
            }
        }
        return GCAP_OK;
    }

    gcap_status_t gcap_frame_to_bgra8(const gcap_frame_packet_t *src, void *dst, int dst_stride)
    {
        if (!src || !dst || dst_stride <= 0 || src->width <= 0 || src->height <= 0 || !src->data[0])
            return GCAP_EINVAL;
        if (dst_stride < src->width * 4)
            return GCAP_EINVAL;

        auto *out = static_cast<uint8_t *>(dst);
        switch (src->format)
        {
        case GCAP_FMT_ARGB:
        {
            const int srcStride = src->stride[0] > 0 ? src->stride[0] : src->width * 4;
            const auto *in = static_cast<const uint8_t *>(src->data[0]);
            for (int y = 0; y < src->height; ++y)
                std::memcpy(out + static_cast<size_t>(y) * dst_stride,
                            in + static_cast<size_t>(y) * srcStride,
                            static_cast<size_t>(src->width) * 4);
            return GCAP_OK;
        }
        case GCAP_FMT_NV12:
        {
            if (src->plane_count < 2 || !src->data[1])
                return GCAP_EINVAL;
            const int yStride = src->stride[0] > 0 ? src->stride[0] : src->width;
            const int uvStride = src->stride[1] > 0 ? src->stride[1] : src->width;
            gcap::nv12_to_argb(static_cast<const uint8_t *>(src->data[0]),
                               static_cast<const uint8_t *>(src->data[1]),
                               src->width, src->height, yStride, uvStride, out, dst_stride);
            return GCAP_OK;
        }
        case GCAP_FMT_YUY2:
        {
            const int srcStride = src->stride[0] > 0 ? src->stride[0] : src->width * 2;
            gcap::yuy2_to_argb(static_cast<const uint8_t *>(src->data[0]),
                               src->width, src->height, srcStride, out, dst_stride);
            return GCAP_OK;
        }
        case GCAP_FMT_Y210:
        {
            const int srcStride = src->stride[0] > 0 ? src->stride[0] : src->width * 4;
            gcap::y210_to_argb(static_cast<const uint8_t *>(src->data[0]),
                               src->width, src->height, srcStride, out, dst_stride);
            return GCAP_OK;
        }
        default:
            return GCAP_ENOTSUP;
        }
    }

    GCAP_API gcap_status_t gcap_enumerate_audio_devices(gcap_audio_device_t *out, int max, int *count)
    {
        if (!out || max <= 0)
            return GCAP_EINVAL;

#ifndef _WIN32
        if (count)
            *count = 0;
        return GCAP_ENOTSUP;
#else
        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);

        ComPtr<IMMDeviceEnumerator> enumerator;
        hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
        if (FAILED(hr))
        {
            if (count)
                *count = 0;
            CoUninitialize();
            return GCAP_EIO;
        }

        // Default endpoint id
        std::wstring defaultId;
        {
            ComPtr<IMMDevice> def;
            if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eCapture, eConsole, &def)))
            {
                wchar_t *wid = nullptr;
                if (SUCCEEDED(def->GetId(&wid)) && wid)
                {
                    defaultId = wid;
                    CoTaskMemFree(wid);
                }
            }
        }

        ComPtr<IMMDeviceCollection> coll;
        hr = enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &coll);
        if (FAILED(hr))
        {
            if (count)
                *count = 0;
            CoUninitialize();
            return GCAP_EIO;
        }

        UINT n = 0;
        coll->GetCount(&n);
        if (count)
            *count = (int)n;

        UINT toCopy = (UINT)max;
        if (toCopy > n)
            toCopy = n;

        for (UINT i = 0; i < toCopy; ++i)
        {
            ComPtr<IMMDevice> dev;
            if (FAILED(coll->Item(i, &dev)))
                continue;

            // id
            wchar_t *wid = nullptr;
            std::wstring idW;
            if (SUCCEEDED(dev->GetId(&wid)) && wid)
            {
                idW = wid;
                CoTaskMemFree(wid);
            }

            // name
            std::wstring nameW;
            ComPtr<IPropertyStore> store;
            if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &store)))
            {
                PROPVARIANT pv;
                PropVariantInit(&pv);
                if (SUCCEEDED(store->GetValue(PKEY_Device_FriendlyName, &pv)))
                {
                    if (pv.vt == VT_LPWSTR && pv.pwszVal)
                        nameW = pv.pwszVal;
                }
                PropVariantClear(&pv);
            }

            std::string idU8 = w2utf8(idW.c_str());
            std::string nameU8 = w2utf8(nameW.c_str());

            memset(&out[i], 0, sizeof(out[i]));
            strncpy(out[i].id, idU8.c_str(), sizeof(out[i].id) - 1);
            strncpy(out[i].name, nameU8.c_str(), sizeof(out[i].name) - 1);
            out[i].is_default = (!defaultId.empty() && idW == defaultId) ? 1 : 0;
        }

        CoUninitialize();
        return GCAP_OK;
#endif
    }

    GCAP_API gcap_status_t gcap_set_recording_audio_device(gcap_handle h, const char *device_id_utf8)
    {
        if (!h)
            return GCAP_EINVAL;
        return h->mgr.setRecordingAudioDevice(device_id_utf8);
    }

    gcap_status_t gcap_stop(gcap_handle h)
    {
        if (!h)
            return GCAP_EINVAL;
        return h->mgr.stop();
    }

    gcap_status_t gcap_close(gcap_handle h)
    {
        if (!h)
            return GCAP_EINVAL;
        // 先停再關（容錯）
        h->mgr.stop();
        gcap_status_t st = h->mgr.close();
        delete h;
        return st;
    }

    gcap_status_t gcap_get_device_props(gcap_handle h, gcap_device_props_t *out)
    {
        if (!h || !out)
            return GCAP_EINVAL;
        return h->mgr.getDeviceProps(*out);
    }

    gcap_status_t gcap_get_signal_status(gcap_handle h, gcap_signal_status_t *out)
    {
        if (!h || !out)
            return GCAP_EINVAL;
        return h->mgr.getSignalStatus(*out);
    }

    GCAP_API gcap_status_t gcap_get_runtime_info(gcap_handle h, gcap_runtime_info_t *out)
    {
        if (!h || !out)
            return GCAP_EINVAL;
        return h->mgr.getRuntimeInfo(*out);
    }

    gcap_status_t gcap_set_processing(gcap_handle h, const gcap_processing_opts_t *opts)
    {
        if (!h || !opts)
            return GCAP_EINVAL;
        return h->mgr.setProcessing(*opts);
    }

    GCAP_API gcap_status_t gcap_get_procamp(gcap_handle h, gcap_procamp_t *out)
    {
        if (!h || !out)
            return GCAP_EINVAL;
        return h->mgr.getProcAmp(*out);
    }

    GCAP_API gcap_status_t gcap_get_procamp_caps(gcap_handle h, gcap_procamp_caps_t *out)
    {
        if (!h || !out)
            return GCAP_EINVAL;
        return h->mgr.getProcAmpCaps(*out);
    }

    GCAP_API gcap_status_t gcap_set_procamp(gcap_handle h, const gcap_procamp_t *p)
    {
        if (!h)
            return GCAP_EINVAL;

        // nullptr means reset to neutral
        gcap_procamp_t v{};
        if (p)
        {
            v = *p;
        }
        else
        {
            v.brightness = 128;
            v.contrast = 128;
            v.hue = 128;
            v.saturation = 128;
            v.sharpness = 128;
        }
        return h->mgr.setProcAmp(v);
    }

    GCAP_API gcap_status_t gcap_reset_procamp(gcap_handle h)
    {
        if (!h)
            return GCAP_EINVAL;
        return h->mgr.resetProcAmp();
    }

    GCAP_API void gcap_set_backend(int backend)
    {
        CaptureManager::setBackendInt(backend);
    }

    GCAP_API void gcap_set_d3d_adapter(int adapter_index)
    {
        CaptureManager::setD3dAdapterInt(adapter_index);
    }

    GCAP_API int gcap_get_active_backend(gcap_handle h)
    {
        if (!h)
            return -1;
        return h->mgr.getActiveBackendInt();
    }

    GCAP_API gcap_status_t gcap_set_preview(gcap_handle h, const gcap_preview_desc_t *desc)
    {
        if (!h || !desc)
            return GCAP_EINVAL;

        return h->mgr.setPreview(*desc);
    }

    GCAP_API gcap_status_t gcap_export_preview_scene_rgb10(gcap_handle h, const char *base_path_utf8,
                                                           int export_raw, int export_tiff, int export_stats)
    {
        if (!h || !base_path_utf8 || !*base_path_utf8)
            return GCAP_EINVAL;
        return h->mgr.exportPreviewSceneRgb10(base_path_utf8,
                                              export_raw != 0 ? GCAP_EXPORT_RAW_ALL : 0,
                                              export_tiff != 0,
                                              export_stats != 0,
                                              false);
    }

    GCAP_API gcap_status_t gcap_export_snapshot(gcap_handle h, const gcap_snapshot_export_desc_t *desc,
                                                gcap_snapshot_export_result_t *out)
    {
        if (!h || !desc || !desc->base_path_utf8 || !*desc->base_path_utf8)
            return GCAP_EINVAL;

        gcap_snapshot_export_result_t local{};
        gcap_snapshot_export_result_t *r = out ? out : &local;
        std::memset(r, 0, sizeof(*r));

        gcap_runtime_info_t rt{};
        h->mgr.getRuntimeInfo(rt);
        r->width = rt.negotiated.width > 0 ? rt.negotiated.width : rt.signal.width;
        r->height = rt.negotiated.height > 0 ? rt.negotiated.height : rt.signal.height;
        r->source_format = rt.negotiated.pixfmt;
        if (gcap_pixfmt_bit_depth(r->source_format) == 0)
            r->source_format = rt.signal.pixfmt;
        r->source_bit_depth = gcap_pixfmt_bit_depth(r->source_format);
        if (r->source_bit_depth <= 0)
            r->source_bit_depth = rt.negotiated.bit_depth > 0 ? rt.negotiated.bit_depth : rt.signal.bit_depth;

        const int flags = desc->flags != 0 ? desc->flags : (GCAP_EXPORT_RAW_ALL | GCAP_EXPORT_TIFF | GCAP_EXPORT_STATS | GCAP_EXPORT_PNG);
        const bool wantRaw = (flags & (GCAP_EXPORT_RAW_ALL | GCAP_EXPORT_RAW_RGBA8)) != 0;
        const bool wantGigabyteRaw = (flags & GCAP_EXPORT_RAW_GIGABYTE_HEADER) != 0;
        const bool wantTiff = (flags & GCAP_EXPORT_TIFF) != 0;
        const bool wantStats = (flags & GCAP_EXPORT_STATS) != 0;
        const bool wantPng = (flags & GCAP_EXPORT_PNG) != 0;

        // PNG is generated by the SDK from the same scene readback RAW data.
        // If the caller requested only PNG, RAW files are created temporarily and
        // removed after PNG encoding succeeds/fails.
        const bool needRawForPng = wantPng;
        int rawFlags = flags & (GCAP_EXPORT_RAW_ALL | GCAP_EXPORT_RAW_RGBA8);
        if (needRawForPng)
            rawFlags |= GCAP_EXPORT_RAW_NATIVE;
        const gcap_status_t st = h->mgr.exportPreviewSceneRgb10(desc->base_path_utf8, rawFlags, wantTiff, wantStats, wantGigabyteRaw);
        if (st != GCAP_OK)
            return st;

        bool sourceIs10Bit = r->source_bit_depth >= 10;
#ifdef _WIN32
        const std::wstring raw10Path = wide_append_suffix(desc->base_path_utf8, L"_abgr2101010.raw");
        const std::wstring raw8Path = wide_append_suffix(desc->base_path_utf8, L"_bgra8.raw");
        const bool raw10Exists = wide_file_exists(raw10Path);
        const bool raw8Exists = wide_file_exists(raw8Path);
        if (raw10Exists || raw8Exists)
        {
            sourceIs10Bit = raw10Exists && !raw8Exists;
            if (raw10Exists && raw8Exists)
                sourceIs10Bit = r->source_bit_depth >= 10;
        }
#endif
        if (wantRaw)
        {
            if (sourceIs10Bit)
            {
                if (flags & GCAP_EXPORT_RAW_NATIVE)
                {
                    copy_path(r->native_raw_path, sizeof(r->native_raw_path), desc->base_path_utf8, "_abgr2101010.raw");
                    r->generated_flags |= GCAP_EXPORT_RAW_NATIVE;
                }
                if (flags & GCAP_EXPORT_RAW_RGB10_U16)
                {
                    copy_path(r->rgb10_u16_path, sizeof(r->rgb10_u16_path), desc->base_path_utf8, "_rgb10.raw");
                    r->generated_flags |= GCAP_EXPORT_RAW_RGB10_U16;
                }
                if (flags & GCAP_EXPORT_RAW_RGBA16)
                {
                    copy_path(r->rgba16_path, sizeof(r->rgba16_path), desc->base_path_utf8, "_rgba16_expanded.raw");
                    r->generated_flags |= GCAP_EXPORT_RAW_RGBA16;
                }
            }
            else
            {
                if (flags & GCAP_EXPORT_RAW_NATIVE)
                {
                    copy_path(r->native_raw_path, sizeof(r->native_raw_path), desc->base_path_utf8, "_bgra8.raw");
                    r->generated_flags |= GCAP_EXPORT_RAW_NATIVE;
                }
                if (flags & GCAP_EXPORT_RAW_RGBA8)
                {
                    copy_path(r->rgba8_path, sizeof(r->rgba8_path), desc->base_path_utf8, "_rgba8.raw");
                    r->generated_flags |= GCAP_EXPORT_RAW_RGBA8;
                }
            }
        }
        if (wantGigabyteRaw)
        {
            if (sourceIs10Bit)
            {
                copy_path(r->gigabyte_native_raw_path, sizeof(r->gigabyte_native_raw_path), desc->base_path_utf8, "_gigabyte_abgr2101010.raw");
            }
            else
            {
                copy_path(r->gigabyte_native_raw_path, sizeof(r->gigabyte_native_raw_path), desc->base_path_utf8, "_gigabyte_bgra8.raw");
            }
            r->generated_flags |= GCAP_EXPORT_RAW_GIGABYTE_HEADER;
        }
        if (wantTiff)
        {
            copy_path(r->tiff_path, sizeof(r->tiff_path), desc->base_path_utf8, ".tiff");
            r->generated_flags |= GCAP_EXPORT_TIFF;
        }
        if (wantStats)
        {
            copy_path(r->stats_path, sizeof(r->stats_path), desc->base_path_utf8, ".stats.txt");
            r->generated_flags |= GCAP_EXPORT_STATS;
        }
#ifdef _WIN32
        if (wantPng)
        {
            const std::wstring pngPath = wide_append_suffix(desc->base_path_utf8, L".png");
            bool pngOk = sourceIs10Bit
                             ? create_png_from_abgr2101010_raw(raw10Path, pngPath, r->width, r->height)
                             : create_png_from_bgra8_raw(raw8Path, pngPath, r->width, r->height);
            if (!pngOk)
            {
                // Be tolerant of drivers/runtime info that report incomplete bit-depth metadata.
                pngOk = sourceIs10Bit
                            ? create_png_from_bgra8_raw(raw8Path, pngPath, r->width, r->height)
                            : create_png_from_abgr2101010_raw(raw10Path, pngPath, r->width, r->height);
            }
            if (pngOk)
            {
                copy_path(r->png_path, sizeof(r->png_path), desc->base_path_utf8, ".png");
                r->generated_flags |= GCAP_EXPORT_PNG;
            }
            if ((flags & GCAP_EXPORT_RAW_NATIVE) == 0)
            {
                DeleteFileW(wide_append_suffix(desc->base_path_utf8, L"_abgr2101010.raw").c_str());
                DeleteFileW(wide_append_suffix(desc->base_path_utf8, L"_bgra8.raw").c_str());
            }
            if (!wantRaw)
            {
                DeleteFileW(wide_append_suffix(desc->base_path_utf8, L"_fp16_rgba16f.raw").c_str());
                DeleteFileW(wide_append_suffix(desc->base_path_utf8, L"_rgb10.raw").c_str());
                DeleteFileW(wide_append_suffix(desc->base_path_utf8, L"_rgb10_u16.raw").c_str());
                DeleteFileW(wide_append_suffix(desc->base_path_utf8, L"_rgba16_expanded.raw").c_str());
                DeleteFileW(wide_append_suffix(desc->base_path_utf8, L"_rgba8.raw").c_str());
            }
            if (!pngOk)
                return GCAP_EIO;
        }
#else
        if (wantPng)
            return GCAP_ENOTSUP;
#endif
        return GCAP_OK;
    }

    extern "C" GCAP_API int gcap_get_audio_device_count(void)
    {
        auto list = gcap::audio::enumerate_devices();
        return static_cast<int>(list.size());
    }

    extern "C" GCAP_API int gcap_enum_audio_devices(
        gcap_audio_device_t *out,
        int max_count)
    {
        auto list = gcap::audio::enumerate_devices();
        int total = static_cast<int>(list.size());

        if (!out || max_count <= 0)
            return total;

        int n = (total < max_count) ? total : max_count;

        for (int i = 0; i < n; ++i)
        {
            const auto &d = list[i];

            memset(&out[i], 0, sizeof(gcap_audio_device_t));
            strncpy_s(out[i].id, d.id.c_str(), GCAP_AUDIO_ID_MAX - 1);
            strncpy_s(out[i].name, d.name.c_str(), GCAP_AUDIO_NAME_MAX - 1);
            out[i].channels = d.channels;
            out[i].sample_rate = d.sample_rate;
            out[i].bits_per_sample = d.bits_per_sample;
            out[i].is_float = d.is_float ? 1 : 0;
            out[i].is_default = d.is_default ? 1 : 0;
        }

        return n;
    }

    extern "C" GCAP_API int gcap_audio_device_count(void)
    {
        return gcap_get_audio_device_count();
    }

    extern "C" GCAP_API int gcap_audio_enum_devices(gcap_audio_device_t *out_devices, int max_devices)
    {
        return gcap_enum_audio_devices(out_devices, max_devices);
    }

    extern "C" GCAP_API int gcap_audio_find_device_for_capture(const char *capture_device_name_utf8,
                                                               gcap_audio_device_t *out_device)
    {
        if (!capture_device_name_utf8 || !*capture_device_name_utf8 || !out_device)
            return 0;

        gcap::audio::device d;
        if (!gcap::audio::find_device_for_capture_name(capture_device_name_utf8, d))
            return 0;

        memset(out_device, 0, sizeof(*out_device));
        strncpy_s(out_device->id, d.id.c_str(), GCAP_AUDIO_ID_MAX - 1);
        strncpy_s(out_device->name, d.name.c_str(), GCAP_AUDIO_NAME_MAX - 1);
        out_device->channels = d.channels;
        out_device->sample_rate = d.sample_rate;
        out_device->bits_per_sample = d.bits_per_sample;
        out_device->is_float = d.is_float ? 1 : 0;
        out_device->is_default = d.is_default ? 1 : 0;
        return 1;
    }

    extern "C" GCAP_API int gcap_start_audio_capture(const gcap_audio_capture_config_t *cfg)
    {
        std::string error;
        const char *deviceId = (cfg && cfg->device_id && *cfg->device_id) ? cfg->device_id : nullptr;
        if (!gcap::audio::start_preview(deviceId, &error))
        {
            if (!error.empty())
                gcap::log_printf(GCAP_LOG_WARN, "[AudioPreview] start failed: %s", error.c_str());
            return GCAP_EIO;
        }
        return GCAP_OK;
    }

    extern "C" GCAP_API int gcap_start_dshow_audio_preview(const char *video_device_name_utf8,
                                                            gcap_audio_device_t *out_source)
    {
        if (!video_device_name_utf8 || !*video_device_name_utf8)
            return GCAP_EINVAL;

        gcap::audio::device source;
        std::string error;
        if (!gcap::audio::start_dshow_preview(video_device_name_utf8, source, &error))
        {
            if (!error.empty())
                gcap::log_printf(GCAP_LOG_WARN, "[AudioPreview][DShow] start failed: %s", error.c_str());
            return GCAP_EIO;
        }

        if (out_source)
        {
            memset(out_source, 0, sizeof(*out_source));
            strncpy_s(out_source->id, source.id.c_str(), _TRUNCATE);
            strncpy_s(out_source->name, source.name.c_str(), _TRUNCATE);
            out_source->channels = source.channels;
            out_source->sample_rate = source.sample_rate;
            out_source->bits_per_sample = source.bits_per_sample;
            out_source->is_float = source.is_float ? 1 : 0;
        }
        return GCAP_OK;
    }

    extern "C" GCAP_API void gcap_stop_audio_capture(void)
    {
        gcap::audio::stop_preview();
    }

} // extern "C"
