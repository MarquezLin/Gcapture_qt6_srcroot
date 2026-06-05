#include "xdma_capture_session.h"

#include "xdma_public.h"

#include <initguid.h>
#include <setupapi.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <new>
#include <sstream>
#include <system_error>

#pragma comment(lib, "SetupAPI.lib")

DEFINE_GUID(GUID_DEVINTERFACE_XDMA,
            0x74c7e4a9, 0x6d5d, 0x4a70, 0xbc, 0x0d, 0x20, 0x69, 0x1d, 0xff, 0x9e, 0x9d);

namespace
{
    constexpr uint32_t kMaxChannels = 2;
    constexpr uint32_t kIrqRolesPerChannel = 4;
    constexpr uint32_t kVideoIrqRole = 0;
    constexpr uint32_t kPlugInIrqRole = 2;
    constexpr uint32_t kPlugOutIrqRole = 3;
    constexpr int kPlugInScanFrames = 5;
    constexpr uint32_t kDefaultWidth = 1920;
    constexpr uint32_t kDefaultHeight = 1080;
    constexpr DWORD kMaxBytesPerTransfer = 0x800000;

    // FPGA user BAR register offsets.  These match the old CaptureDemo
    // register map: write capture-enable to start/stop DMA, read width/height
    // to infer the current input signal, and pulse INT_CLR after an IRQ.
    constexpr long kInterruptClearReg = 0x500;
    constexpr long kVideo0CaptureEnableReg = 0x004;
    constexpr long kVideo1CaptureEnableReg = 0x304;
    constexpr long kVideoFormatReg = 0x0c;
    constexpr long kVideoWidthReg = 0x10;
    constexpr long kVideoHeightReg = 0x14;
    constexpr long kVideoFrameRateReg = 0x18;
    constexpr long kVideoBitDepthReg = 0x1c;
    constexpr long kFpgaStatusReg = 0x180;
    constexpr long kPlugInFrameFixReg = 0x200;

    static uint32_t bit_n(uint32_t n)
    {
        return n < 32 ? (1u << n) : 0u;
    }

    static bool should_log_counter(uint64_t count)
    {
        return count <= 5 || (count % 60) == 0;
    }

    static bool is_stream_event_role(uint32_t role)
    {
        return role == kVideoIrqRole || role == kPlugInIrqRole || role == kPlugOutIrqRole;
    }

    static uint32_t event_mask_for_type(xdma_event_type_t type)
    {
        switch (type)
        {
        case XDMA_EVENT_VIDEO_IRQ:
            return XDMA_EVENT_MASK_VIDEO_IRQ;
        case XDMA_EVENT_PLUG_IN:
            return XDMA_EVENT_MASK_PLUG_IN;
        case XDMA_EVENT_PLUG_OUT:
            return XDMA_EVENT_MASK_PLUG_OUT;
        case XDMA_EVENT_CAPTURE_PAUSED:
            return XDMA_EVENT_MASK_CAPTURE_PAUSED;
        case XDMA_EVENT_CAPTURE_RESUMED:
            return XDMA_EVENT_MASK_CAPTURE_RESUMED;
        default:
            return 0;
        }
    }

    static uint64_t steady_now_ns()
    {
        const auto now = std::chrono::steady_clock::now().time_since_epoch();
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
    }

    static size_t find_frame_marker_offset(const uint8_t *data, size_t bytes)
    {
        if (!data || bytes < 2)
            return bytes;
        for (size_t i = 0; i + 1 < bytes; ++i)
        {
            if (data[i] == 0xff && data[i + 1] == 0xff)
                return i;
        }
        return bytes;
    }

    static uint32_t decode_bit_depth(uint32_t raw, uint32_t fallback)
    {
        const uint32_t value = raw & 0xffu;
        return (value == 8u || value == 10u) ? value : fallback;
    }

    static gdriver_pixel_format_t decode_pixel_format(uint32_t rawFormat, uint32_t bitDepth)
    {
        (void)bitDepth;
        switch (rawFormat & 0x3u)
        {
        case 0:
            return GDRIVER_PIXFMT_YUY2;
        case 1:
            return GDRIVER_PIXFMT_RGB24;
        case 2:
            return GDRIVER_PIXFMT_YUV444;
        case 3:
            return GDRIVER_PIXFMT_NV12;
        default:
            return GDRIVER_PIXFMT_UNKNOWN;
        }
    }

    static uint32_t bit_depth_for_pixfmt(gdriver_pixel_format_t fmt)
    {
        switch (fmt)
        {
        case GDRIVER_PIXFMT_P010:
        case GDRIVER_PIXFMT_Y210:
            return 10;
        default:
            return 8;
        }
    }

    static size_t bytes_per_frame(uint32_t width, uint32_t height, gdriver_pixel_format_t fmt, uint32_t bitDepth)
    {
        const size_t pixels = static_cast<size_t>(width) * static_cast<size_t>(height);
        switch (fmt)
        {
        case GDRIVER_PIXFMT_NV12:
            return pixels * 3u / 2u;
        case GDRIVER_PIXFMT_P010:
            return pixels * 3u;
        case GDRIVER_PIXFMT_RGB24:
        case GDRIVER_PIXFMT_YUV444:
            return bitDepth > 8u ? pixels * 6u : pixels * 3u;
        case GDRIVER_PIXFMT_Y210:
            return pixels * 4u;
        case GDRIVER_PIXFMT_YUY2:
        case GDRIVER_PIXFMT_UYVY:
        default:
            return pixels * 2u;
        }
    }

    static uint32_t plane_count_for_pixfmt(gdriver_pixel_format_t fmt)
    {
        return (fmt == GDRIVER_PIXFMT_NV12 || fmt == GDRIVER_PIXFMT_P010) ? 2u : 1u;
    }

    static uint32_t plane_stride_for_pixfmt(uint32_t width, gdriver_pixel_format_t fmt, uint32_t bitDepth)
    {
        switch (fmt)
        {
        case GDRIVER_PIXFMT_NV12:
            return width;
        case GDRIVER_PIXFMT_P010:
            return width * 2u;
        case GDRIVER_PIXFMT_RGB24:
        case GDRIVER_PIXFMT_YUV444:
            return bitDepth > 8u ? width * 6u : width * 3u;
        case GDRIVER_PIXFMT_Y210:
            return width * 4u;
        case GDRIVER_PIXFMT_YUY2:
        case GDRIVER_PIXFMT_UYVY:
        default:
            return width * 2u;
        }
    }

#if defined(GVFG_XDMA_DEBUG_LOG)
    static void xdma_debug_log(const char *fmt, ...)
    {
        char msg[1024] = {};
        va_list args;
        va_start(args, fmt);
        vsnprintf(msg, sizeof(msg), fmt, args);
        va_end(args);

        char line[1152] = {};
        snprintf(line, sizeof(line), "[GVFG][XDMA] %s\n", msg);
        OutputDebugStringA(line);
    }
#define XDMA_LOG(...) xdma_debug_log(__VA_ARGS__)
#else
#define XDMA_LOG(...) ((void)0)
#endif

    static void xdma_hotplug_log(const char *fmt, ...)
    {
        char msg[1024] = {};
        va_list args;
        va_start(args, fmt);
        vsnprintf(msg, sizeof(msg), fmt, args);
        va_end(args);

        char line[1152] = {};
        snprintf(line, sizeof(line), "[GVFG][XDMA][hotplug] %s\n", msg);
        OutputDebugStringA(line);
    }
#define XDMA_HOTPLUG_LOG(...) xdma_hotplug_log(__VA_ARGS__)

    static std::string wide_to_utf8(const std::wstring &s)
    {
        if (s.empty())
            return std::string();
        const int needed = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, nullptr, 0, nullptr, nullptr);
        if (needed <= 0)
            return std::string();
        std::string out(static_cast<size_t>(needed), '\0');
        WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, &out[0], needed, nullptr, nullptr);
        if (!out.empty() && out.back() == '\0')
            out.pop_back();
        return out;
    }

    static std::string summarize_device_path(const std::wstring &path)
    {
        std::string s = wide_to_utf8(path);
        std::string lower = s;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c)
                       { return static_cast<char>(std::tolower(c)); });

        const auto pciPos = lower.find("pci#");
        const auto guidPos = lower.find("#{");
        if (pciPos == std::string::npos)
            return s;

        const std::string pci = s.substr(pciPos, guidPos == std::string::npos ? std::string::npos : guidPos - pciPos);
        const auto venPos = lower.find("ven_", pciPos);
        const auto devPos = lower.find("dev_", pciPos);
        const auto subsysPos = lower.find("subsys_", pciPos);
        const auto revPos = lower.find("rev_", pciPos);
        if (venPos == std::string::npos || devPos == std::string::npos)
            return pci;

        std::ostringstream oss;
        oss << "pci ven=" << s.substr(venPos + 4, 4)
            << " dev=" << s.substr(devPos + 4, 4);
        if (subsysPos != std::string::npos)
            oss << " subsys=" << s.substr(subsysPos + 7, 8);
        if (revPos != std::string::npos)
            oss << " rev=" << s.substr(revPos + 4, 2);
        return oss.str();
    }

    static void copy_string(const std::string &src, char *dst, size_t dstSize)
    {
        if (!dst || dstSize == 0)
            return;
        dst[0] = '\0';
        if (src.empty())
            return;
        const size_t n = (std::min)(src.size(), dstSize - 1);
        std::memcpy(dst, src.data(), n);
        dst[n] = '\0';
    }

    static void reset_stats(xdma_stream_stats_t &stats, gdriver_stream_state_t state)
    {
        std::memset(&stats, 0, sizeof(stats));
        stats.state = state;
    }

    static std::string win32_error(DWORD err)
    {
        char *msg = nullptr;
        const DWORD len = FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER |
                                             FORMAT_MESSAGE_FROM_SYSTEM |
                                             FORMAT_MESSAGE_IGNORE_INSERTS,
                                         nullptr,
                                         err,
                                         MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                                         reinterpret_cast<LPSTR>(&msg),
                                         0,
                                         nullptr);
        std::string out;
        if (len && msg)
        {
            out.assign(msg, msg + len);
            while (!out.empty() && (out.back() == '\r' || out.back() == '\n' || out.back() == ' '))
                out.pop_back();
        }
        if (msg)
            LocalFree(msg);
        if (out.empty())
        {
            std::ostringstream oss;
            oss << "win32=" << err;
            out = oss.str();
        }
        return out;
    }
}

namespace gvfg::internal
{
    // Enumerate XDMA device interface paths with Windows SetupAPI.
    std::vector<XdmaDevice> enumerate_xdma_devices()
    {
        std::vector<XdmaDevice> devices;
        XDMA_LOG("enumerate: begin guid=74c7e4a9-6d5d-4a70-bc0d-20691dff9e9d");

        // Find installed XDMA device interfaces.  This is the SDK version of
        // CaptureDemo::getDevices(): keep the Windows SetupAPI detail hidden
        // inside the XDMA backend so customer code only calls gvfg_enumerate_devices().
        HDEVINFO info = SetupDiGetClassDevsW(&GUID_DEVINTERFACE_XDMA, nullptr, nullptr,
                                             DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
        if (info == INVALID_HANDLE_VALUE)
        {
            XDMA_LOG("enumerate: SetupDiGetClassDevsW failed gle=%lu", GetLastError());
            return devices;
        }

        SP_DEVICE_INTERFACE_DATA iface = {};
        iface.cbSize = sizeof(iface);
        for (DWORD index = 0; SetupDiEnumDeviceInterfaces(info, nullptr, &GUID_DEVINTERFACE_XDMA, index, &iface); ++index)
        {
            DWORD required = 0;
            SetupDiGetDeviceInterfaceDetailW(info, &iface, nullptr, 0, &required, nullptr);
            if (required == 0)
                continue;

            std::vector<uint8_t> detailBytes(required);
            auto *detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W *>(detailBytes.data());
            detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
            if (!SetupDiGetDeviceInterfaceDetailW(info, &iface, detail, required, nullptr, nullptr))
                continue;

            XdmaDevice device;
            device.interface_path = detail->DevicePath;
            device.friendly_name = L"XDMA Capture Device " + std::to_wstring(devices.size());
            XDMA_LOG("enumerate: device[%zu] %s",
                     devices.size(),
                     summarize_device_path(device.interface_path).c_str());
            devices.push_back(device);
        }

        SetupDiDestroyDeviceInfoList(info);
        XDMA_LOG("enumerate: done count=%zu", devices.size());
        return devices;
    }

    XdmaCaptureSession::XdmaCaptureSession()
    {
        stream_desc_.channel_index = 0;
        stream_desc_.input = GDRIVER_INPUT_SDI;
        stream_desc_.width = kDefaultWidth;
        stream_desc_.height = kDefaultHeight;
        stream_desc_.pixel_format = GDRIVER_PIXFMT_YUY2;
        stream_bit_depth_ = 8;
        stream_desc_.buffer_count = 1;
        stream_desc_.memory_kind = GDRIVER_MEMORY_DRIVER_COPY;
        reset_stats(stats_, GDRIVER_STREAM_STOPPED);
    }

    XdmaCaptureSession::~XdmaCaptureSession()
    {
        close();
    }

    xdma_status_t XdmaCaptureSession::open_default()
    {
        const auto devices = enumerate_xdma_devices();
        XDMA_LOG("open_default: device_count=%zu", devices.size());
        if (devices.empty())
            return fail(XDMA_ENODEV, "enumerate_xdma_devices", ERROR_NOT_FOUND);
        return open_device(devices.front());
    }

    xdma_status_t XdmaCaptureSession::open_device_index(size_t deviceIndex)
    {
        const auto devices = enumerate_xdma_devices();
        XDMA_LOG("open_device_index: requested=%zu device_count=%zu", deviceIndex, devices.size());
        if (deviceIndex >= devices.size())
            return fail(XDMA_ENODEV, "enumerate_xdma_devices", ERROR_NOT_FOUND);
        return open_device(devices[deviceIndex]);
    }

    xdma_status_t XdmaCaptureSession::open_device(const XdmaDevice &device)
    {
        XDMA_LOG("open_device: begin friendly=%s %s",
                 wide_to_utf8(device.friendly_name).c_str(),
                 summarize_device_path(device.interface_path).c_str());
        close();
        base_path_ = device.interface_path;
        friendly_name_ = device.friendly_name.empty() ? L"XDMA Capture Device" : device.friendly_name;

        // XDMA exposes several subdevices under the same base path:
        //   user  : FPGA register access through ReadFile/WriteFile
        //   c2h_N : card-to-host DMA data path for video channel N
        //   event_N is opened later in start_stream(), only for the active IRQ.
        user_device_ = open_subdevice(L"user");
        if (user_device_ == INVALID_HANDLE_VALUE)
        {
            close_handles();
            return fail(XDMA_EIO, "CreateFile(user)");
        }

        for (uint32_t ch = 0; ch < kMaxChannels; ++ch)
        {
            const std::wstring name = L"c2h_" + std::to_wstring(ch);
            c2h_device_[ch] = open_subdevice(name.c_str());
            if (c2h_device_[ch] == INVALID_HANDLE_VALUE)
            {
                close_handles();
                return fail(XDMA_EIO, "CreateFile(c2h)");
            }
            XDMA_LOG("open_device: c2h_%u ready", ch);
        }

        opened_ = true;
        configured_ = false;
        clear_last_error();
        XDMA_LOG("open_device: done");
        return XDMA_OK;
    }

    xdma_status_t XdmaCaptureSession::close()
    {
        XDMA_LOG("close: begin opened=%d configured=%d running=%d",
                 opened_ ? 1 : 0,
                 configured_ ? 1 : 0,
                 running_ ? 1 : 0);
        stop_stream();
        close_handles();
        opened_ = false;
        configured_ = false;
        base_path_.clear();
        friendly_name_.clear();
        XDMA_LOG("close: done");
        return XDMA_OK;
    }

    void XdmaCaptureSession::close_handles()
    {
        for (uint32_t role = 0; role < kIrqRolesPerChannel; ++role)
        {
            if (event_device_[role] != INVALID_HANDLE_VALUE)
            {
                CloseHandle(event_device_[role]);
                event_device_[role] = INVALID_HANDLE_VALUE;
                XDMA_LOG("close_handles: event_%u closed", role);
            }
        }
        if (user_device_ != INVALID_HANDLE_VALUE)
        {
            CloseHandle(user_device_);
            user_device_ = INVALID_HANDLE_VALUE;
            XDMA_LOG("close_handles: user closed");
        }
        for (HANDLE &h : c2h_device_)
        {
            if (h != INVALID_HANDLE_VALUE)
            {
                CloseHandle(h);
                h = INVALID_HANDLE_VALUE;
                XDMA_LOG("close_handles: c2h closed");
            }
        }
    }

    HANDLE XdmaCaptureSession::open_subdevice(const wchar_t *name) const
    {
        if (base_path_.empty() || !name || !*name)
            return INVALID_HANDLE_VALUE;

        // Full path example:
        //   <xdma device interface path>\user
        //   <xdma device interface path>\c2h_0
        //   <xdma device interface path>\event_0
        const std::wstring path = base_path_ + L"\\" + name;
        HANDLE h = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE)
        {
            XDMA_LOG("open_subdevice: %s failed gle=%lu path=%s",
                     wide_to_utf8(name).c_str(),
                     GetLastError(),
                     wide_to_utf8(path).c_str());
        }
        else
        {
            XDMA_LOG("open_subdevice: %s ready", wide_to_utf8(name).c_str());
        }
        return h;
    }

    xdma_status_t XdmaCaptureSession::set_input(gdriver_input_t input, uint32_t channelIndex)
    {
        if (!opened_ || channelIndex >= kMaxChannels)
            return XDMA_EINVAL;
        input_ = input == GDRIVER_INPUT_UNKNOWN ? GDRIVER_INPUT_SDI : input;
        stream_desc_.input = input_;
        stream_desc_.channel_index = channelIndex;
        XDMA_LOG("set_input: input=%u channel=%u", static_cast<unsigned>(input_), channelIndex);
        return XDMA_OK;
    }

    xdma_status_t XdmaCaptureSession::get_device_info(xdma_device_info_t &out) const
    {
        if (!opened_)
            return XDMA_ESTATE;
        std::memset(&out, 0, sizeof(out));
        copy_string(wide_to_utf8(friendly_name_), out.friendly_name, sizeof(out.friendly_name));
        copy_string("XDMA", out.driver_version, sizeof(out.driver_version));
        out.supported_inputs_mask = (1u << GDRIVER_INPUT_SDI) | (1u << GDRIVER_INPUT_HDMI);
        out.supported_pixel_formats_mask = (1u << GDRIVER_PIXFMT_YUY2) |
                                           (1u << GDRIVER_PIXFMT_Y210) |
                                           (1u << GDRIVER_PIXFMT_RGB24) |
                                           (1u << GDRIVER_PIXFMT_NV12) |
                                           (1u << GDRIVER_PIXFMT_P010) |
                                           (1u << GDRIVER_PIXFMT_YUV444);
        out.max_video_channels = kMaxChannels;
        out.max_audio_channels = 0;
        return XDMA_OK;
    }

    xdma_status_t XdmaCaptureSession::get_signal_status(xdma_signal_status_t &out) const
    {
        if (!opened_)
            return XDMA_ESTATE;
        std::memset(&out, 0, sizeof(out));
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t rawFormat = 0;
        uint32_t rawFrameRate = 0;
        uint32_t rawBitDepth = 0;
        uint32_t rawStatus = 0;

        // The current XDMA backend has no high-level signal IOCTL yet, so we
        // read the FPGA signal registers directly.  If width/height are empty,
        // fall back to the configured/default stream size.
        const bool formatOk = read_user_reg(kVideoFormatReg, rawFormat);
        const bool widthOk = read_user_reg(kVideoWidthReg, width);
        const bool heightOk = read_user_reg(kVideoHeightReg, height);
        const bool frameRateOk = read_user_reg(kVideoFrameRateReg, rawFrameRate);
        const bool bitDepthOk = read_user_reg(kVideoBitDepthReg, rawBitDepth);
        const bool statusOk = read_user_reg(kFpgaStatusReg, rawStatus);

        const uint32_t signalBitDepth = bitDepthOk ? decode_bit_depth(rawBitDepth, 8) : 0;
        const gdriver_pixel_format_t pixelFormat = formatOk ? decode_pixel_format(rawFormat, signalBitDepth) : stream_desc_.pixel_format;
        const uint32_t bufferBitDepth = bit_depth_for_pixfmt(pixelFormat);

        XDMA_LOG("signal: fmt_ok=%d fmt_raw=0x%x fps_ok=%d fps_raw=0x%x bit_ok=%d bit_raw=%u status_ok=%d status=0x%08x width_ok=%d width=%u height_ok=%d height=%u configured=%ux%u fmt=%u",
                 formatOk ? 1 : 0,
                 rawFormat,
                 frameRateOk ? 1 : 0,
                 rawFrameRate,
                 bitDepthOk ? 1 : 0,
                 rawBitDepth,
                 statusOk ? 1 : 0,
                 rawStatus,
                 widthOk ? 1 : 0,
                 width,
                 heightOk ? 1 : 0,
                 height,
                 stream_desc_.width,
                 stream_desc_.height,
                 static_cast<unsigned>(stream_desc_.pixel_format));

        const bool haveSignalSize = widthOk && heightOk && width != 0 && height != 0;
        const bool sdiLocked = statusOk ? ((rawStatus & bit_n(0)) != 0) : haveSignalSize;
        const bool hdmiLocked = statusOk ? ((rawStatus & bit_n(2)) != 0) : haveSignalSize;
        out.signal_locked = (input_ == GDRIVER_INPUT_HDMI ? hdmiLocked : sdiLocked) ? 1 : 0;
        out.input = input_;
        out.width = haveSignalSize ? width : stream_desc_.width;
        out.height = haveSignalSize ? height : stream_desc_.height;
        out.pixel_format = pixelFormat;
        out.bit_depth = bufferBitDepth;
        out.fpga_valid_mask = (formatOk ? bit_n(0) : 0u) |
                              (frameRateOk ? bit_n(1) : 0u) |
                              (bitDepthOk ? bit_n(2) : 0u) |
                              (statusOk ? bit_n(3) : 0u);
        out.fpga_width_valid = widthOk ? 1u : 0u;
        out.fpga_height_valid = heightOk ? 1u : 0u;
        out.fpga_width_raw = width;
        out.fpga_height_raw = height;
        out.fpga_video_format_raw = rawFormat;
        out.fpga_frame_rate_raw = rawFrameRate;
        out.fpga_bit_depth_raw = rawBitDepth;
        out.fpga_status_raw = rawStatus;
        return XDMA_OK;
    }

    xdma_status_t XdmaCaptureSession::get_stream_stats(xdma_stream_stats_t &out) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        out = stats_;
        out.state = running_ ? GDRIVER_STREAM_RUNNING : (configured_ ? GDRIVER_STREAM_CONFIGURED : GDRIVER_STREAM_STOPPED);
        return XDMA_OK;
    }

    xdma_status_t XdmaCaptureSession::set_event_callback(xdma_event_callback_t callback, void *user, uint32_t eventMask)
    {
        std::lock_guard<std::mutex> lock(event_callback_mutex_);
        event_callback_ = callback;
        event_callback_user_ = user;
        event_mask_filter_ = eventMask ? eventMask : XDMA_EVENT_MASK_DEFAULT;
        return XDMA_OK;
    }

    xdma_status_t XdmaCaptureSession::configure_stream(const xdma_stream_desc_t &desc)
    {
        if (!opened_)
            return XDMA_ESTATE;
        if (running_)
            return XDMA_ESTATE;
        if (desc.channel_index >= kMaxChannels)
            return fail(XDMA_EINVAL, "configure_stream(channel)", ERROR_INVALID_PARAMETER);
        if (desc.pixel_format != GDRIVER_PIXFMT_YUY2 &&
            desc.pixel_format != GDRIVER_PIXFMT_Y210 &&
            desc.pixel_format != GDRIVER_PIXFMT_RGB24 &&
            desc.pixel_format != GDRIVER_PIXFMT_NV12 &&
            desc.pixel_format != GDRIVER_PIXFMT_P010 &&
            desc.pixel_format != GDRIVER_PIXFMT_YUV444 &&
            desc.pixel_format != GDRIVER_PIXFMT_UNKNOWN)
            return fail(XDMA_ENOTSUP, "configure_stream(pixel_format)", ERROR_NOT_SUPPORTED);

        XDMA_LOG("configure: request ch=%u input=%u %ux%u fmt=%u buffers=%u mem=%u flags=0x%x",
                 desc.channel_index,
                 static_cast<unsigned>(desc.input),
                 desc.width,
                 desc.height,
                 static_cast<unsigned>(desc.pixel_format),
                 desc.buffer_count,
                 static_cast<unsigned>(desc.memory_kind),
                 desc.flags);

        // Normalize the requested stream.  The render path may support fewer
        // formats than the FPGA, but the DMA frame size must follow the signal.
        stream_desc_ = desc;
        stream_desc_.input = desc.input == GDRIVER_INPUT_UNKNOWN ? input_ : desc.input;
        stream_desc_.width = desc.width ? desc.width : kDefaultWidth;
        stream_desc_.height = desc.height ? desc.height : kDefaultHeight;
        stream_desc_.pixel_format = desc.pixel_format == GDRIVER_PIXFMT_UNKNOWN ? GDRIVER_PIXFMT_YUY2 : desc.pixel_format;
        uint32_t rawBitDepth = 0;
        stream_bit_depth_ = bit_depth_for_pixfmt(stream_desc_.pixel_format);
        stream_desc_.buffer_count = desc.buffer_count ? desc.buffer_count : 1;
        stream_desc_.memory_kind = GDRIVER_MEMORY_DRIVER_COPY;
        configured_ = true;

        {
            std::lock_guard<std::mutex> lock(mutex_);
            reset_stats(stats_, GDRIVER_STREAM_CONFIGURED);
            latest_frame_.clear();
            delivery_frame_.clear();
            latest_sequence_ = 0;
            delivered_sequence_ = 0;
            wait_timeout_count_ = 0;
        }

        XDMA_LOG("configure: effective ch=%u input=%u %ux%u fmt=%u frame_bytes=%zu",
                 stream_desc_.channel_index,
                 static_cast<unsigned>(stream_desc_.input),
                 stream_desc_.width,
                 stream_desc_.height,
                 static_cast<unsigned>(stream_desc_.pixel_format),
                 frame_size_bytes());
        return XDMA_OK;
    }

    xdma_status_t XdmaCaptureSession::start_stream()
    {
        XDMA_LOG("start: begin opened=%d configured=%d running=%d",
                 opened_ ? 1 : 0,
                 configured_ ? 1 : 0,
                 running_ ? 1 : 0);
        if (!opened_ || !configured_)
            return XDMA_ESTATE;
        if (running_)
            return XDMA_OK;

        const uint32_t ch = active_channel();
        if (c2h_device_[ch] == INVALID_HANDLE_VALUE)
            return fail(XDMA_EIO, "c2h handle", ERROR_INVALID_HANDLE);

        for (uint32_t role = 0; role < kIrqRolesPerChannel; ++role)
        {
            if (event_device_[role] != INVALID_HANDLE_VALUE)
            {
                CloseHandle(event_device_[role]);
                event_device_[role] = INVALID_HANDLE_VALUE;
            }
        }

        // IRQ layout follows CaptureDemo: each video channel owns four event
        // bits: VIDEO_INT, AUDIO_INT, PLUG_IN, PLUG_OUT.
        for (uint32_t role = 0; role < kIrqRolesPerChannel; ++role)
        {
            if (!is_stream_event_role(role))
                continue;

            const std::wstring eventName = L"event_" + std::to_wstring(ch * kIrqRolesPerChannel + role);
            event_device_[role] = open_subdevice(eventName.c_str());
            if (event_device_[role] == INVALID_HANDLE_VALUE)
            {
                for (uint32_t openedRole = 0; openedRole < kIrqRolesPerChannel; ++openedRole)
                {
                    if (event_device_[openedRole] != INVALID_HANDLE_VALUE)
                    {
                        CloseHandle(event_device_[openedRole]);
                        event_device_[openedRole] = INVALID_HANDLE_VALUE;
                    }
                }
                return fail(XDMA_EIO, "CreateFile(event)");
            }
        }

        const size_t bytes = frame_size_bytes();
        if (bytes == 0)
            return fail(XDMA_EINVAL, "frame_size_bytes", ERROR_INVALID_PARAMETER);
        XDMA_LOG("start: ch=%u event_mask=0x%08x capture_reg=0x%lx frame_bytes=%zu",
                 ch,
                 active_event_mask(),
                 capture_enable_reg(),
                 bytes);

        {
            std::lock_guard<std::mutex> lock(mutex_);
            dma_buffer_.assign(bytes, 0);
            latest_frame_.clear();
            delivery_frame_.clear();
            pending_events_ = 0;
            latest_sequence_ = 0;
            delivered_sequence_ = 0;
            wait_timeout_count_ = 0;
            stream_error_ = false;
            reset_stats(stats_, GDRIVER_STREAM_RUNNING);
        }
        save_frames_after_plug_in_.store(0);
        fix_pulsed_after_plug_in_.store(false);

        // Match CaptureDemo's hot-plug-safe startup order: make the event/data
        // workers ready first, then clear stale IRQs, toggle capture, and only
        // then enable user interrupt delivery.
        running_ = true;
        capture_active_ = true;
        try
        {
            for (uint32_t role = 0; role < kIrqRolesPerChannel; ++role)
            {
                if (!is_stream_event_role(role))
                    continue;

                const uint32_t irqBit = ch * kIrqRolesPerChannel + role;
                event_thread_[role] = std::thread([this, role, irqBit]()
                                                  {
                                                      try
                                                      {
                                                          event_thread_proc(role, irqBit);
                                                      }
                                                      catch (...)
                                                      {
                                                          running_ = false;
                                                          capture_active_ = false;
                                                          data_cv_.notify_all();
                                                          frame_cv_.notify_all();
                                                      }
                                                  });
            }
            if (!start_data_worker())
                throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again));

            Sleep(10);
            XDMA_LOG("start: clear event mask=0x%08x", active_event_mask());
            write_user_reg(kInterruptClearReg, active_event_mask());
            write_user_reg(kInterruptClearReg, 0);
            XDMA_LOG("start: capture disable reg=0x%lx", capture_enable_reg());
            write_user_reg(capture_enable_reg(), 0);
            XDMA_LOG("start: capture enable reg=0x%lx", capture_enable_reg());
            write_user_reg(capture_enable_reg(), 1);
            if (!enable_user_event(active_event_mask()))
                throw std::system_error(std::make_error_code(std::errc::io_error));
            XDMA_LOG("start: user event enabled mask=0x%08x", active_event_mask());
        }
        catch (const std::system_error &)
        {
            running_ = false;
            capture_active_ = false;
            disable_user_event(active_event_mask());
            write_user_reg(capture_enable_reg(), 0);
            for (HANDLE h : event_device_)
            {
                if (h != INVALID_HANDLE_VALUE)
                    CancelIoEx(h, nullptr);
            }
            if (ch < kMaxChannels && c2h_device_[ch] != INVALID_HANDLE_VALUE)
                CancelIoEx(c2h_device_[ch], nullptr);
            data_cv_.notify_all();
            frame_cv_.notify_all();
            for (std::thread &thread : event_thread_)
            {
                if (thread.joinable())
                    thread.join();
            }
            stop_data_worker();
            return fail(XDMA_EIO, "start worker thread", ERROR_NOT_ENOUGH_MEMORY);
        }
        XDMA_LOG("start: threads launched");
        return XDMA_OK;
    }

    xdma_status_t XdmaCaptureSession::stop_stream()
    {
        const bool wasRunning = running_.exchange(false);
        capture_active_ = false;
        XDMA_LOG("stop: begin was_running=%d", wasRunning ? 1 : 0);
        if (!wasRunning)
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stats_.state = configured_ ? GDRIVER_STREAM_CONFIGURED : GDRIVER_STREAM_STOPPED;
            XDMA_LOG("stop: no active stream state=%u", static_cast<unsigned>(stats_.state));
            return XDMA_OK;
        }

        // Stop in the reverse direction: disable IRQ/capture first, then cancel
        // blocking ReadFile calls so worker threads can exit and join cleanly.
        XDMA_LOG("stop: disable event mask=0x%08x capture_reg=0x%lx", active_event_mask(), capture_enable_reg());
        disable_user_event(active_event_mask());
        write_user_reg(capture_enable_reg(), 0);

        for (HANDLE h : event_device_)
        {
            if (h != INVALID_HANDLE_VALUE)
                CancelIoEx(h, nullptr);
        }
        const uint32_t ch = active_channel();
        if (ch < kMaxChannels && c2h_device_[ch] != INVALID_HANDLE_VALUE)
            CancelIoEx(c2h_device_[ch], nullptr);

        {
            std::lock_guard<std::mutex> lock(mutex_);
            pending_events_ = 0;
        }
        data_cv_.notify_all();
        frame_cv_.notify_all();

        for (std::thread &thread : event_thread_)
        {
            if (thread.joinable())
                thread.join();
        }
        stop_data_worker();

        std::lock_guard<std::mutex> lock(mutex_);
        stats_.state = configured_ ? GDRIVER_STREAM_CONFIGURED : GDRIVER_STREAM_STOPPED;
        XDMA_LOG("stop: done captured=%llu delivered=%llu interrupts=%llu dma_errors=%llu",
                 static_cast<unsigned long long>(stats_.frames_captured),
                 static_cast<unsigned long long>(stats_.frames_delivered),
                 static_cast<unsigned long long>(stats_.interrupt_count),
                 static_cast<unsigned long long>(stats_.dma_errors));
        return XDMA_OK;
    }

    xdma_status_t XdmaCaptureSession::wait_frame(uint32_t timeoutMs, xdma_frame_t &out)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        std::memset(&out, 0, sizeof(out));
        if (!running_)
            return XDMA_ESTATE;

        const auto hasFrame = [this]()
        {
            return latest_sequence_ > delivered_sequence_ || stream_error_ || !running_;
        };

        if (timeoutMs == 0)
        {
            frame_cv_.wait(lock, hasFrame);
        }
        else if (!frame_cv_.wait_for(lock, std::chrono::milliseconds(timeoutMs), hasFrame))
        {
            ++wait_timeout_count_;
            if (should_log_counter(wait_timeout_count_))
                XDMA_LOG("wait_frame: timeout count=%llu timeout_ms=%u latest=%llu delivered=%llu running=%d stream_error=%d",
                         static_cast<unsigned long long>(wait_timeout_count_),
                         timeoutMs,
                         static_cast<unsigned long long>(latest_sequence_),
                         static_cast<unsigned long long>(delivered_sequence_),
                         running_ ? 1 : 0,
                         stream_error_ ? 1 : 0);
            return XDMA_ETIMEOUT;
        }

        if (!running_ && latest_sequence_ <= delivered_sequence_)
        {
            XDMA_LOG("wait_frame: stopped without pending frame");
            return XDMA_ESTATE;
        }
        if (stream_error_ && latest_sequence_ <= delivered_sequence_)
        {
            XDMA_LOG("wait_frame: stream error latest=%llu delivered=%llu",
                     static_cast<unsigned long long>(latest_sequence_),
                     static_cast<unsigned long long>(delivered_sequence_));
            return XDMA_EIO;
        }
        if (latest_sequence_ <= delivered_sequence_)
            return XDMA_ETIMEOUT;

        // Return a stable pointer to delivery_frame_. The pointer is valid
        // until the next wait_frame() on this same session or close().
        delivery_frame_ = latest_frame_;
        delivered_sequence_ = latest_sequence_;
        ++stats_.frames_delivered;

        out.data = delivery_frame_.data();
        out.data_size_bytes = delivery_frame_.size();
        out.frame_id = delivered_sequence_;
        out.timestamp_ns = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count());
        out.width = stream_desc_.width;
        out.height = stream_desc_.height;
        out.pixel_format = stream_desc_.pixel_format;
        out.bit_depth = stream_bit_depth_;
        out.plane_count = plane_count_for_pixfmt(stream_desc_.pixel_format);
        out.plane_offset_bytes[0] = 0;
        out.plane_stride_bytes[0] = plane_stride_for_pixfmt(stream_desc_.width, stream_desc_.pixel_format, stream_bit_depth_);
        if (out.plane_count > 1)
        {
            out.plane_offset_bytes[1] = out.plane_stride_bytes[0] * stream_desc_.height;
            out.plane_stride_bytes[1] = out.plane_stride_bytes[0];
        }
        out.driver_buffer_index = 0;
        out.flags = 0;
        if (should_log_counter(out.frame_id))
            XDMA_LOG("wait_frame: deliver id=%llu bytes=%zu %ux%u stride=%u captured=%llu delivered=%llu",
                     static_cast<unsigned long long>(out.frame_id),
                     out.data_size_bytes,
                     out.width,
                     out.height,
                     out.plane_stride_bytes[0],
                     static_cast<unsigned long long>(stats_.frames_captured),
                     static_cast<unsigned long long>(stats_.frames_delivered));
        return XDMA_OK;
    }

    xdma_status_t XdmaCaptureSession::release_frame(const xdma_frame_t &)
    {
        return XDMA_OK;
    }

    void XdmaCaptureSession::emit_event(xdma_event_type_t type, uint32_t irqBit, uint32_t irqMask) const
    {
        xdma_event_callback_t callback = nullptr;
        void *user = nullptr;
        {
            std::lock_guard<std::mutex> lock(event_callback_mutex_);
            const uint32_t typeMask = event_mask_for_type(type);
            if (!event_callback_ || typeMask == 0 || (event_mask_filter_ & typeMask) == 0)
                return;
            callback = event_callback_;
            user = event_callback_user_;
        }

        xdma_event_t event{};
        event.type = type;
        event.channel = active_channel();
        event.irq_bit = irqBit;
        event.irq_mask = irqMask;
        event.timestamp_ns = steady_now_ns();
        callback(&event, user);
    }

    void XdmaCaptureSession::event_thread_proc(uint32_t role, uint32_t irqBit)
    {
        const uint32_t mask = bit_n(irqBit);
        XDMA_LOG("event_thread: start role=%u irq=%u mask=0x%08x", role, irqBit, mask);
        while (running_)
        {
            uint8_t value = 0;

            // Blocking read from event_N.  The XDMA driver completes this read
            // when the FPGA raises the selected user interrupt.
            const int ret = read_device(event_device_[role], 0, 1, &value);
            if (!running_)
                break;
            if (ret < 0)
            {
                fail(XDMA_EIO, "ReadFile(event)");
                std::lock_guard<std::mutex> lock(mutex_);
                stream_error_ = true;
                ++stats_.dma_errors;
                frame_cv_.notify_all();
                break;
            }

            uint64_t interruptCount = 0;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                ++stats_.interrupt_count;
                interruptCount = stats_.interrupt_count;
            }

            if (role == kPlugOutIrqRole)
            {
                XDMA_HOTPLUG_LOG("PLUG_OUT IRQ ch=%u irq=%u mask=0x%08x value=%u interrupt_count=%llu active=%d running=%d",
                                  active_channel(),
                                  irqBit,
                                  mask,
                                  static_cast<unsigned>(value),
                                  static_cast<unsigned long long>(interruptCount),
                                  capture_active_.load() ? 1 : 0,
                                  running_.load() ? 1 : 0);
                XDMA_LOG("event_thread: PLUG_OUT irq=%u value=%u", irqBit, static_cast<unsigned>(value));
                emit_event(XDMA_EVENT_PLUG_OUT, irqBit, mask);
                pause_capture_for_plug_out();
                continue;
            }

            if (role == kPlugInIrqRole)
            {
                XDMA_HOTPLUG_LOG("PLUG_IN IRQ ch=%u irq=%u mask=0x%08x value=%u interrupt_count=%llu active=%d running=%d",
                                  active_channel(),
                                  irqBit,
                                  mask,
                                  static_cast<unsigned>(value),
                                  static_cast<unsigned long long>(interruptCount),
                                  capture_active_.load() ? 1 : 0,
                                  running_.load() ? 1 : 0);
                XDMA_LOG("event_thread: PLUG_IN irq=%u value=%u", irqBit, static_cast<unsigned>(value));
                emit_event(XDMA_EVENT_PLUG_IN, irqBit, mask);
                resume_capture_after_plug_in();
                continue;
            }

            if (role != kVideoIrqRole)
            {
                write_user_reg(kInterruptClearReg, mask);
                write_user_reg(kInterruptClearReg, 0);
                if (capture_active_)
                    enable_user_event(mask);
                else
                    enable_user_event(plug_in_event_mask());
                continue;
            }

            uint32_t pending = 0;
            if (capture_active_)
            {
                std::lock_guard<std::mutex> lock(mutex_);
                ++pending_events_;
                pending = pending_events_;
            }
            else
            {
                write_user_reg(kInterruptClearReg, mask);
                write_user_reg(kInterruptClearReg, 0);
                enable_user_event(plug_in_event_mask());
                continue;
            }

            if (should_log_counter(interruptCount))
                XDMA_LOG("event_thread: irq=%llu role=%u value=%u pending=%u",
                         static_cast<unsigned long long>(interruptCount),
                         role,
                         static_cast<unsigned>(value),
                         pending);
            emit_event(XDMA_EVENT_VIDEO_IRQ, irqBit, mask);
            data_cv_.notify_one();
        }
        XDMA_LOG("event_thread: exit role=%u running=%d", role, running_ ? 1 : 0);
    }

    void XdmaCaptureSession::data_thread_proc()
    {
        XDMA_LOG("data_thread: start ch=%u frame_bytes=%zu", active_channel(), frame_size_bytes());
        uint64_t readCount = 0;
        while (running_ && !data_worker_stop_)
        {
            uint32_t pendingAfterPop = 0;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                data_cv_.wait(lock, [this]()
                              { return (capture_active_ && pending_events_ > 0) || !running_ || data_worker_stop_; });
                if (!running_ || data_worker_stop_)
                    break;
                if (!capture_active_)
                    continue;
                --pending_events_;
                pendingAfterPop = pending_events_;
            }

            ++readCount;
            if (should_log_counter(readCount))
                XDMA_LOG("data_thread: event consumed read=%llu pending=%u clear_reg=0x%lx mask=0x%08x",
                         static_cast<unsigned long long>(readCount),
                         pendingAfterPop,
                         kInterruptClearReg,
                         video_event_mask());
            // A VIDEO_INT means one frame is ready in C2H.  Acknowledge the FPGA
            // interrupt, re-enable XDMA user interrupt delivery, then read the
            // full video frame from c2h_N.
            write_user_reg(kInterruptClearReg, video_event_mask());
            write_user_reg(kInterruptClearReg, 0);
            enable_user_event(video_event_mask());

            if (!capture_active_)
                continue;

            const DWORD bytes = static_cast<DWORD>(frame_size_bytes());
            if (dma_buffer_.size() < bytes)
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (dma_buffer_.size() < bytes)
                    dma_buffer_.resize(bytes);
            }
            const int ret = read_device(c2h_device_[active_channel()], 0, bytes, dma_buffer_.data());

            if (!running_)
                break;
            if (!capture_active_)
                continue;
            if (ret < 0 || static_cast<DWORD>(ret) != bytes)
            {
                fail(XDMA_EIO, "ReadFile(c2h)");
                std::lock_guard<std::mutex> lock(mutex_);
                stream_error_ = true;
                ++stats_.dma_errors;
                frame_cv_.notify_all();
                break;
            }

            handle_plug_in_frame_fix(dma_buffer_.data(), static_cast<size_t>(ret));

            std::lock_guard<std::mutex> lock(mutex_);
            publish_frame(dma_buffer_.data(), static_cast<size_t>(ret));
        }
        XDMA_LOG("data_thread: exit running=%d", running_ ? 1 : 0);
    }

    bool XdmaCaptureSession::start_data_worker()
    {
        std::lock_guard<std::mutex> lock(worker_mutex_);
        if (data_thread_.joinable())
            return true;

        data_worker_stop_ = false;
        try
        {
            data_thread_ = std::thread([this]()
                                       {
                                           try
                                           {
                                               data_thread_proc();
                                           }
                                           catch (...)
                                           {
                                               running_ = false;
                                               capture_active_ = false;
                                               frame_cv_.notify_all();
                                           }
                                       });
            return true;
        }
        catch (const std::system_error &)
        {
            return false;
        }
    }

    void XdmaCaptureSession::stop_data_worker()
    {
        data_worker_stop_ = true;
        const uint32_t ch = active_channel();
        if (ch < kMaxChannels && c2h_device_[ch] != INVALID_HANDLE_VALUE)
            CancelIoEx(c2h_device_[ch], nullptr);
        data_cv_.notify_all();

        std::thread thread;
        {
            std::lock_guard<std::mutex> lock(worker_mutex_);
            if (!data_thread_.joinable())
                return;
            thread = std::move(data_thread_);
        }

        if (thread.joinable())
            thread.join();
    }

    void XdmaCaptureSession::pause_capture_for_plug_out()
    {
        if (!capture_active_.exchange(false))
        {
            const bool clearHighOk = write_user_reg(kInterruptClearReg, plug_out_event_mask());
            const bool clearLowOk = write_user_reg(kInterruptClearReg, 0);
            const bool enablePlugInOk = enable_user_event(plug_in_event_mask());
            XDMA_HOTPLUG_LOG("PLUG_OUT pause skipped ch=%u already_inactive clear_out=%d/%d enable_plug_in=%d",
                              active_channel(),
                              clearHighOk ? 1 : 0,
                              clearLowOk ? 1 : 0,
                              enablePlugInOk ? 1 : 0);
            return;
        }

        XDMA_HOTPLUG_LOG("PLUG_OUT pause begin ch=%u active_mask=0x%08x video_mask=0x%08x plug_in_mask=0x%08x plug_out_mask=0x%08x capture_reg=0x%lx",
                          active_channel(),
                          active_event_mask(),
                          video_event_mask(),
                          plug_in_event_mask(),
                          plug_out_event_mask(),
                          capture_enable_reg());
        XDMA_LOG("hotplug: plug-out pause ch=%u active_mask=0x%08x plug_in_mask=0x%08x",
                 active_channel(),
                 active_event_mask(),
                 plug_in_event_mask());

        const bool captureOffOk = write_user_reg(capture_enable_reg(), 0);
        const bool disableActiveOk = disable_user_event(active_event_mask());
        const bool clearActiveHighOk = write_user_reg(kInterruptClearReg, active_event_mask());
        const bool clearActiveLowOk = write_user_reg(kInterruptClearReg, 0);
        const bool enablePlugInOk = enable_user_event(plug_in_event_mask());

        // Firmware raises the next PLUG_IN IRQ only while capture-enable is high.
        const bool captureArmedOk = write_user_reg(capture_enable_reg(), running_ ? 1u : 0u);
        XDMA_HOTPLUG_LOG("PLUG_OUT pause regs ch=%u capture_off=%d disable_active=%d clear_active=%d/%d enable_plug_in=%d capture_armed=%d armed_value=%u",
                          active_channel(),
                          captureOffOk ? 1 : 0,
                          disableActiveOk ? 1 : 0,
                          clearActiveHighOk ? 1 : 0,
                          clearActiveLowOk ? 1 : 0,
                          enablePlugInOk ? 1 : 0,
                          captureArmedOk ? 1 : 0,
                          running_.load() ? 1u : 0u);

        {
            std::lock_guard<std::mutex> lock(mutex_);
            pending_events_ = 0;
        }
        stop_data_worker();
        uint64_t latestAfterPause = 0;
        uint64_t deliveredAfterPause = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            dma_buffer_.clear();
            latest_frame_.clear();
            delivery_frame_.clear();
            latest_sequence_ = delivered_sequence_;
            latestAfterPause = latest_sequence_;
            deliveredAfterPause = delivered_sequence_;
        }
        frame_cv_.notify_all();
        emit_event(XDMA_EVENT_CAPTURE_PAUSED,
                   active_channel() * kIrqRolesPerChannel + kPlugOutIrqRole,
                   plug_out_event_mask());
        XDMA_HOTPLUG_LOG("PLUG_OUT pause done ch=%u active=%d data_worker_stop=%d latest_sequence=%llu delivered_sequence=%llu",
                          active_channel(),
                          capture_active_.load() ? 1 : 0,
                          data_worker_stop_.load() ? 1 : 0,
                          static_cast<unsigned long long>(latestAfterPause),
                          static_cast<unsigned long long>(deliveredAfterPause));
    }

    void XdmaCaptureSession::resume_capture_after_plug_in()
    {
        if (capture_active_.load())
        {
            const bool clearHighOk = write_user_reg(kInterruptClearReg, plug_in_event_mask());
            const bool clearLowOk = write_user_reg(kInterruptClearReg, 0);
            bool enableActiveOk = true;
            if (running_)
                enableActiveOk = enable_user_event(active_event_mask());
            XDMA_HOTPLUG_LOG("PLUG_IN resume skipped ch=%u already_active clear_in=%d/%d enable_active=%d active_mask=0x%08x",
                              active_channel(),
                              clearHighOk ? 1 : 0,
                              clearLowOk ? 1 : 0,
                              enableActiveOk ? 1 : 0,
                              active_event_mask());
            return;
        }

        save_frames_after_plug_in_.store(kPlugInScanFrames);
        fix_pulsed_after_plug_in_.store(false);

        XDMA_HOTPLUG_LOG("PLUG_IN resume begin ch=%u active_mask=0x%08x video_mask=0x%08x plug_in_mask=0x%08x plug_out_mask=0x%08x capture_reg=0x%lx scan_frames=%d",
                          active_channel(),
                          active_event_mask(),
                          video_event_mask(),
                          plug_in_event_mask(),
                          plug_out_event_mask(),
                          capture_enable_reg(),
                          kPlugInScanFrames);
        XDMA_LOG("hotplug: plug-in resume ch=%u active_mask=0x%08x",
                 active_channel(),
                 active_event_mask());

        size_t allocatedBytes = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const size_t bytes = frame_size_bytes();
            dma_buffer_.assign(bytes, 0);
            allocatedBytes = bytes;
            pending_events_ = 0;
            stream_error_ = false;
            stats_.state = GDRIVER_STREAM_RUNNING;
        }
        XDMA_HOTPLUG_LOG("PLUG_IN resume buffer ch=%u frame_bytes=%zu", active_channel(), allocatedBytes);

        capture_active_ = true;
        if (!start_data_worker())
        {
            capture_active_ = false;
            std::lock_guard<std::mutex> lock(mutex_);
            stream_error_ = true;
            ++stats_.dma_errors;
            frame_cv_.notify_all();
            XDMA_HOTPLUG_LOG("PLUG_IN resume failed ch=%u start_data_worker=0", active_channel());
            return;
        }
        Sleep(10);

        const bool captureOffOk = write_user_reg(capture_enable_reg(), 0);
        const bool clearActiveHighOk = write_user_reg(kInterruptClearReg, active_event_mask());
        const bool clearActiveLowOk = write_user_reg(kInterruptClearReg, 0);
        if (!running_)
        {
            XDMA_HOTPLUG_LOG("PLUG_IN resume aborted ch=%u running=0 capture_off=%d clear_active=%d/%d",
                              active_channel(),
                              captureOffOk ? 1 : 0,
                              clearActiveHighOk ? 1 : 0,
                              clearActiveLowOk ? 1 : 0);
            return;
        }
        const bool captureOnOk = write_user_reg(capture_enable_reg(), 1);
        const bool enableActiveOk = enable_user_event(active_event_mask());
        data_cv_.notify_all();
        frame_cv_.notify_all();
        emit_event(XDMA_EVENT_CAPTURE_RESUMED,
                   active_channel() * kIrqRolesPerChannel + kPlugInIrqRole,
                   plug_in_event_mask());
        XDMA_HOTPLUG_LOG("PLUG_IN resume done ch=%u capture_off=%d clear_active=%d/%d capture_on=%d enable_active=%d active=%d active_mask=0x%08x",
                          active_channel(),
                          captureOffOk ? 1 : 0,
                          clearActiveHighOk ? 1 : 0,
                          clearActiveLowOk ? 1 : 0,
                          captureOnOk ? 1 : 0,
                          enableActiveOk ? 1 : 0,
                          capture_active_.load() ? 1 : 0,
                          active_event_mask());
    }

    void XdmaCaptureSession::handle_plug_in_frame_fix(const uint8_t *data, size_t bytes)
    {
        int remaining = save_frames_after_plug_in_.load();
        if (remaining <= 0 || fix_pulsed_after_plug_in_.load())
            return;

        while (remaining > 0 && !save_frames_after_plug_in_.compare_exchange_weak(remaining, remaining - 1))
        {
        }
        if (remaining <= 0)
            return;

        const size_t markerOffset = find_frame_marker_offset(data, bytes);
        if (markerOffset >= bytes)
        {
            XDMA_HOTPLUG_LOG("PLUG_IN frame scan ch=%u marker_not_found remaining_before=%d bytes=%zu",
                              active_channel(),
                              remaining,
                              bytes);
            return;
        }

        XDMA_HOTPLUG_LOG("PLUG_IN frame marker found ch=%u offset=%zu remaining_before=%d bytes=%zu",
                          active_channel(),
                          markerOffset,
                          remaining,
                          bytes);
        XDMA_LOG("hotplug: plug-in frame marker found offset=%zu remaining=%d", markerOffset, remaining);
        pulse_plug_in_frame_fix();
    }

    void XdmaCaptureSession::pulse_plug_in_frame_fix()
    {
        bool expected = false;
        if (!fix_pulsed_after_plug_in_.compare_exchange_strong(expected, true))
            return;

        uint32_t statusAfterHigh = 0;
        uint32_t statusAfterLow = 0;
        XDMA_LOG("hotplug: pulse PLUG_IN_FRAME_FIX reg=0x%lx", kPlugInFrameFixReg);
        const bool highOk = write_user_reg(kPlugInFrameFixReg, 1);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        const bool readHighOk = read_user_reg(kFpgaStatusReg, statusAfterHigh);
        const bool lowOk = write_user_reg(kPlugInFrameFixReg, 0);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        const bool readLowOk = read_user_reg(kFpgaStatusReg, statusAfterLow);
        XDMA_HOTPLUG_LOG("PLUG_IN_FRAME_FIX pulse ch=%u reg=0x%lx high=%d status_high=%d/0x%08x low=%d status_low=%d/0x%08x",
                          active_channel(),
                          kPlugInFrameFixReg,
                          highOk ? 1 : 0,
                          readHighOk ? 1 : 0,
                          statusAfterHigh,
                          lowOk ? 1 : 0,
                          readLowOk ? 1 : 0,
                          statusAfterLow);
    }

    void XdmaCaptureSession::publish_frame(const uint8_t *data, size_t bytes)
    {
        // Keep only the newest frame.  This makes wait_frame() simple and avoids
        // unbounded queue growth if the consumer is slower than the device.
        latest_frame_.assign(data, data + bytes);
        ++latest_sequence_;
        ++stats_.frames_captured;
        stats_.state = GDRIVER_STREAM_RUNNING;
        if (should_log_counter(latest_sequence_))
            XDMA_LOG("publish_frame: id=%llu bytes=%zu captured=%llu",
                     static_cast<unsigned long long>(latest_sequence_),
                     bytes,
                     static_cast<unsigned long long>(stats_.frames_captured));
        frame_cv_.notify_one();
    }

    int XdmaCaptureSession::read_device(HANDLE device, long address, DWORD size, uint8_t *buffer) const
    {
        if (device == INVALID_HANDLE_VALUE || !buffer)
            return -1;
        if (SetFilePointer(device, address, nullptr, FILE_BEGIN) == INVALID_SET_FILE_POINTER && GetLastError() != NO_ERROR)
        {
            XDMA_LOG("read_device: SetFilePointer failed handle=%p addr=0x%lx size=%lu gle=%lu",
                     device,
                     address,
                     size,
                     GetLastError());
            return -3;
        }

        // XDMA reads/writes are file operations at a BAR/DMA offset.  Large
        // transfers are split so a full HD/4K frame does not exceed the
        // driver's per-request size limit.
        DWORD done = 0;
        DWORD offset = 0;
        while (offset + kMaxBytesPerTransfer <= size)
        {
            if (!ReadFile(device, buffer + offset, kMaxBytesPerTransfer, &done, nullptr))
            {
                XDMA_LOG("read_device: ReadFile chunk failed handle=%p addr=0x%lx offset=%lu size=%lu gle=%lu",
                         device,
                         address,
                         offset,
                         kMaxBytesPerTransfer,
                         GetLastError());
                return -1;
            }
            if (done != kMaxBytesPerTransfer)
            {
                XDMA_LOG("read_device: short chunk handle=%p addr=0x%lx offset=%lu got=%lu expected=%lu",
                         device,
                         address,
                         offset,
                         done,
                         kMaxBytesPerTransfer);
                return -2;
            }
            offset += kMaxBytesPerTransfer;
        }

        const DWORD remaining = size - offset;
        if (remaining)
        {
            if (!ReadFile(device, buffer + offset, remaining, &done, nullptr))
            {
                XDMA_LOG("read_device: ReadFile tail failed handle=%p addr=0x%lx offset=%lu size=%lu gle=%lu",
                         device,
                         address,
                         offset,
                         remaining,
                         GetLastError());
                return -1;
            }
            if (done != remaining)
            {
                XDMA_LOG("read_device: short tail handle=%p addr=0x%lx offset=%lu got=%lu expected=%lu",
                         device,
                         address,
                         offset,
                         done,
                         remaining);
                return -2;
            }
        }
        return static_cast<int>(size);
    }

    int XdmaCaptureSession::write_device(HANDLE device, long address, DWORD size, const uint8_t *buffer) const
    {
        if (device == INVALID_HANDLE_VALUE || !buffer)
            return -1;
        if (SetFilePointer(device, address, nullptr, FILE_BEGIN) == INVALID_SET_FILE_POINTER && GetLastError() != NO_ERROR)
        {
            XDMA_LOG("write_device: SetFilePointer failed handle=%p addr=0x%lx size=%lu gle=%lu",
                     device,
                     address,
                     size,
                     GetLastError());
            return -3;
        }

        DWORD done = 0;
        DWORD offset = 0;
        while (offset + kMaxBytesPerTransfer <= size)
        {
            if (!WriteFile(device, buffer + offset, kMaxBytesPerTransfer, &done, nullptr))
            {
                XDMA_LOG("write_device: WriteFile chunk failed handle=%p addr=0x%lx offset=%lu size=%lu gle=%lu",
                         device,
                         address,
                         offset,
                         kMaxBytesPerTransfer,
                         GetLastError());
                return -1;
            }
            if (done != kMaxBytesPerTransfer)
            {
                XDMA_LOG("write_device: short chunk handle=%p addr=0x%lx offset=%lu got=%lu expected=%lu",
                         device,
                         address,
                         offset,
                         done,
                         kMaxBytesPerTransfer);
                return -2;
            }
            offset += kMaxBytesPerTransfer;
        }

        const DWORD remaining = size - offset;
        if (remaining)
        {
            if (!WriteFile(device, buffer + offset, remaining, &done, nullptr))
            {
                XDMA_LOG("write_device: WriteFile tail failed handle=%p addr=0x%lx offset=%lu size=%lu gle=%lu",
                         device,
                         address,
                         offset,
                         remaining,
                         GetLastError());
                return -1;
            }
            if (done != remaining)
            {
                XDMA_LOG("write_device: short tail handle=%p addr=0x%lx offset=%lu got=%lu expected=%lu",
                         device,
                         address,
                         offset,
                         done,
                         remaining);
                return -2;
            }
        }
        return static_cast<int>(size);
    }

    bool XdmaCaptureSession::read_user_reg(long address, uint32_t &out) const
    {
        out = 0;
        // User BAR register read: SetFilePointer(user, address) + ReadFile(4).
        const bool ok = read_device(user_device_, address, sizeof(out), reinterpret_cast<uint8_t *>(&out)) == sizeof(out);
        if (!ok)
            XDMA_LOG("read_user_reg: addr=0x%lx failed", address);
        return ok;
    }

    bool XdmaCaptureSession::write_user_reg(long address, uint32_t value) const
    {
        // User BAR register write: SetFilePointer(user, address) + WriteFile(4).
        const bool ok = write_device(user_device_, address, sizeof(value), reinterpret_cast<const uint8_t *>(&value)) == sizeof(value);
        if (!ok)
            XDMA_LOG("write_user_reg: addr=0x%lx value=0x%08x failed", address, value);
        return ok;
    }

    bool XdmaCaptureSession::enable_user_event(uint32_t mask)
    {
        static std::atomic<uint64_t> enableLogCount{0};
        DWORD returned = 0;
        // This is one of the few DeviceIoControl calls in the direct XDMA path:
        // it tells the XDMA driver which user interrupt bits should wake event_N.
        const bool ok = DeviceIoControl(user_device_, IOCTL_XDMA_USER_INT_ENABLE,
                                        &mask, sizeof(mask), nullptr, 0, &returned, nullptr) != FALSE;
        const uint64_t count = ++enableLogCount;
        if (!ok || should_log_counter(count))
            XDMA_LOG("enable_user_event: count=%llu mask=0x%08x ok=%d gle=%lu",
                     static_cast<unsigned long long>(count),
                     mask,
                     ok ? 1 : 0,
                     ok ? 0 : GetLastError());
        return ok;
    }

    bool XdmaCaptureSession::disable_user_event(uint32_t mask)
    {
        DWORD returned = 0;
        const bool ok = DeviceIoControl(user_device_, IOCTL_XDMA_USER_INT_DISABLE,
                                        &mask, sizeof(mask), nullptr, 0, &returned, nullptr) != FALSE;
        XDMA_LOG("disable_user_event: mask=0x%08x ok=%d gle=%lu", mask, ok ? 1 : 0, ok ? 0 : GetLastError());
        return ok;
    }

    xdma_status_t XdmaCaptureSession::fail(xdma_status_t status, const char *where, DWORD winerr) const
    {
        std::ostringstream oss;
        oss << (where ? where : "XDMA") << " failed";
        if (winerr != NO_ERROR)
            oss << ": " << win32_error(winerr) << " (" << winerr << ")";
        set_last_error(oss.str());
        XDMA_LOG("error: status=%u %s", static_cast<unsigned>(status), oss.str().c_str());
        return status;
    }

    void XdmaCaptureSession::set_last_error(const std::string &message) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        last_error_ = message;
    }

    void XdmaCaptureSession::clear_last_error() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        last_error_.clear();
    }

    const char *XdmaCaptureSession::last_error() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return last_error_.c_str();
    }

    uint32_t XdmaCaptureSession::active_channel() const
    {
        return (std::min)(stream_desc_.channel_index, kMaxChannels - 1);
    }

    uint32_t XdmaCaptureSession::event_mask(uint32_t role) const
    {
        return bit_n(active_channel() * kIrqRolesPerChannel + role);
    }

    uint32_t XdmaCaptureSession::video_event_mask() const
    {
        return event_mask(kVideoIrqRole);
    }

    uint32_t XdmaCaptureSession::plug_in_event_mask() const
    {
        return event_mask(kPlugInIrqRole);
    }

    uint32_t XdmaCaptureSession::plug_out_event_mask() const
    {
        return event_mask(kPlugOutIrqRole);
    }

    uint32_t XdmaCaptureSession::active_event_mask() const
    {
        return video_event_mask() | plug_in_event_mask() | plug_out_event_mask();
    }

    long XdmaCaptureSession::capture_enable_reg() const
    {
        return active_channel() == 0 ? kVideo0CaptureEnableReg : kVideo1CaptureEnableReg;
    }

    size_t XdmaCaptureSession::frame_size_bytes() const
    {
        return bytes_per_frame(stream_desc_.width, stream_desc_.height, stream_desc_.pixel_format, stream_bit_depth_);
    }
}
