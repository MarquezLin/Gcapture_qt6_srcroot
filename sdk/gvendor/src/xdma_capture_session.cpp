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

#pragma comment(lib, "SetupAPI.lib")

DEFINE_GUID(GUID_DEVINTERFACE_XDMA,
            0x74c7e4a9, 0x6d5d, 0x4a70, 0xbc, 0x0d, 0x20, 0x69, 0x1d, 0xff, 0x9e, 0x9d);

namespace
{
    constexpr uint32_t kMaxChannels = 2;
    constexpr uint32_t kDefaultWidth = 1920;
    constexpr uint32_t kDefaultHeight = 1080;
    constexpr DWORD kMaxBytesPerTransfer = 0x800000;

    constexpr long kInterruptClearReg = 0x500;
    constexpr long kVideo0CaptureEnableReg = 0x004;
    constexpr long kVideo1CaptureEnableReg = 0x304;
    constexpr long kVideoWidthReg = 0x10;
    constexpr long kVideoHeightReg = 0x14;

    static uint32_t bit_n(uint32_t n)
    {
        return n < 32 ? (1u << n) : 0u;
    }

    static bool should_log_counter(uint64_t count)
    {
        return count <= 5 || (count % 60) == 0;
    }

#if defined(GVENDOR_XDMA_DEBUG_LOG)
    static void xdma_debug_log(const char *fmt, ...)
    {
        char msg[1024] = {};
        va_list args;
        va_start(args, fmt);
        vsnprintf(msg, sizeof(msg), fmt, args);
        va_end(args);

        char line[1152] = {};
        snprintf(line, sizeof(line), "[GVendor][XDMA] %s\n", msg);
        OutputDebugStringA(line);
    }
#define XDMA_LOG(...) xdma_debug_log(__VA_ARGS__)
#else
#define XDMA_LOG(...) ((void)0)
#endif

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
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });

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

    static void reset_stats(gv_stream_stats_t &stats, gdriver_stream_state_t state)
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

namespace gvendor
{
    std::vector<XdmaDevice> enumerate_xdma_devices()
    {
        std::vector<XdmaDevice> devices;
        XDMA_LOG("enumerate: begin guid=74c7e4a9-6d5d-4a70-bc0d-20691dff9e9d");
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
        stream_desc_.fps_num = 30000;
        stream_desc_.fps_den = 1001;
        stream_desc_.pixel_format = GDRIVER_PIXFMT_YUY2;
        stream_desc_.buffer_count = 1;
        stream_desc_.memory_kind = GDRIVER_MEMORY_DRIVER_COPY;
        reset_stats(stats_, GDRIVER_STREAM_STOPPED);
    }

    XdmaCaptureSession::~XdmaCaptureSession()
    {
        close();
    }

    gv_status_t XdmaCaptureSession::open_default()
    {
        const auto devices = enumerate_xdma_devices();
        XDMA_LOG("open_default: device_count=%zu", devices.size());
        if (devices.empty())
            return fail(GV_ENODEV, "enumerate_xdma_devices", ERROR_NOT_FOUND);
        return open_device(devices.front());
    }

    gv_status_t XdmaCaptureSession::open_device_index(size_t deviceIndex)
    {
        const auto devices = enumerate_xdma_devices();
        XDMA_LOG("open_device_index: requested=%zu device_count=%zu", deviceIndex, devices.size());
        if (deviceIndex >= devices.size())
            return fail(GV_ENODEV, "enumerate_xdma_devices", ERROR_NOT_FOUND);
        return open_device(devices[deviceIndex]);
    }

    gv_status_t XdmaCaptureSession::open_device(const XdmaDevice &device)
    {
        XDMA_LOG("open_device: begin friendly=%s %s",
                 wide_to_utf8(device.friendly_name).c_str(),
                 summarize_device_path(device.interface_path).c_str());
        close();
        base_path_ = device.interface_path;
        friendly_name_ = device.friendly_name.empty() ? L"XDMA Capture Device" : device.friendly_name;

        user_device_ = open_subdevice(L"user");
        if (user_device_ == INVALID_HANDLE_VALUE)
        {
            close_handles();
            return fail(GV_EIO, "CreateFile(user)");
        }

        for (uint32_t ch = 0; ch < kMaxChannels; ++ch)
        {
            const std::wstring name = L"c2h_" + std::to_wstring(ch);
            c2h_device_[ch] = open_subdevice(name.c_str());
            if (c2h_device_[ch] == INVALID_HANDLE_VALUE)
            {
                close_handles();
                return fail(GV_EIO, "CreateFile(c2h)");
            }
            XDMA_LOG("open_device: c2h_%u ready", ch);
        }

        opened_ = true;
        configured_ = false;
        clear_last_error();
        XDMA_LOG("open_device: done");
        return GV_OK;
    }

    gv_status_t XdmaCaptureSession::close()
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
        return GV_OK;
    }

    void XdmaCaptureSession::close_handles()
    {
        if (event_device_ != INVALID_HANDLE_VALUE)
        {
            CloseHandle(event_device_);
            event_device_ = INVALID_HANDLE_VALUE;
            XDMA_LOG("close_handles: event closed");
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

    gv_status_t XdmaCaptureSession::set_input(gdriver_input_t input, uint32_t channelIndex)
    {
        if (!opened_ || channelIndex >= kMaxChannels)
            return GV_EINVAL;
        input_ = input == GDRIVER_INPUT_UNKNOWN ? GDRIVER_INPUT_SDI : input;
        stream_desc_.input = input_;
        stream_desc_.channel_index = channelIndex;
        XDMA_LOG("set_input: input=%u channel=%u", static_cast<unsigned>(input_), channelIndex);
        return GV_OK;
    }

    gv_status_t XdmaCaptureSession::get_device_info(gv_device_info_t &out) const
    {
        if (!opened_)
            return GV_ESTATE;
        std::memset(&out, 0, sizeof(out));
        copy_string(wide_to_utf8(friendly_name_), out.friendly_name, sizeof(out.friendly_name));
        copy_string("XDMA", out.driver_version, sizeof(out.driver_version));
        out.supported_inputs_mask = (1u << GDRIVER_INPUT_SDI) | (1u << GDRIVER_INPUT_HDMI);
        out.supported_pixel_formats_mask = (1u << GDRIVER_PIXFMT_YUY2);
        out.max_video_channels = kMaxChannels;
        out.max_audio_channels = 0;
        return GV_OK;
    }

    gv_status_t XdmaCaptureSession::get_signal_status(gv_signal_status_t &out) const
    {
        if (!opened_)
            return GV_ESTATE;
        std::memset(&out, 0, sizeof(out));
        uint32_t width = 0;
        uint32_t height = 0;
        const bool widthOk = read_user_reg(kVideoWidthReg, width);
        const bool heightOk = read_user_reg(kVideoHeightReg, height);
        XDMA_LOG("signal: width_ok=%d width=%u height_ok=%d height=%u configured=%ux%u fmt=%u",
                 widthOk ? 1 : 0,
                 width,
                 heightOk ? 1 : 0,
                 height,
                 stream_desc_.width,
                 stream_desc_.height,
                 static_cast<unsigned>(stream_desc_.pixel_format));

        const bool haveSignalSize = widthOk && heightOk && width != 0 && height != 0;
        out.signal_locked = haveSignalSize ? 1 : 0;
        out.input = input_;
        out.width = haveSignalSize ? width : stream_desc_.width;
        out.height = haveSignalSize ? height : stream_desc_.height;
        out.fps_num = stream_desc_.fps_num;
        out.fps_den = stream_desc_.fps_den;
        out.pixel_format = stream_desc_.pixel_format;
        out.bit_depth = 8;
        return GV_OK;
    }

    gv_status_t XdmaCaptureSession::get_stream_stats(gv_stream_stats_t &out) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        out = stats_;
        out.state = running_ ? GDRIVER_STREAM_RUNNING : (configured_ ? GDRIVER_STREAM_CONFIGURED : GDRIVER_STREAM_STOPPED);
        return GV_OK;
    }

    gv_status_t XdmaCaptureSession::configure_stream(const gv_stream_desc_t &desc)
    {
        if (!opened_)
            return GV_ESTATE;
        if (running_)
            return GV_ESTATE;
        if (desc.channel_index >= kMaxChannels)
            return fail(GV_EINVAL, "configure_stream(channel)", ERROR_INVALID_PARAMETER);
        if (desc.pixel_format != GDRIVER_PIXFMT_YUY2 && desc.pixel_format != GDRIVER_PIXFMT_UNKNOWN)
            return fail(GV_ENOTSUP, "configure_stream(pixel_format)", ERROR_NOT_SUPPORTED);

        XDMA_LOG("configure: request ch=%u input=%u %ux%u fps=%u/%u fmt=%u buffers=%u mem=%u flags=0x%x",
                 desc.channel_index,
                 static_cast<unsigned>(desc.input),
                 desc.width,
                 desc.height,
                 desc.fps_num,
                 desc.fps_den,
                 static_cast<unsigned>(desc.pixel_format),
                 desc.buffer_count,
                 static_cast<unsigned>(desc.memory_kind),
                 desc.flags);

        stream_desc_ = desc;
        stream_desc_.input = desc.input == GDRIVER_INPUT_UNKNOWN ? input_ : desc.input;
        stream_desc_.width = desc.width ? desc.width : kDefaultWidth;
        stream_desc_.height = desc.height ? desc.height : kDefaultHeight;
        stream_desc_.fps_num = desc.fps_num ? desc.fps_num : 30000;
        stream_desc_.fps_den = desc.fps_den ? desc.fps_den : 1001;
        stream_desc_.pixel_format = GDRIVER_PIXFMT_YUY2;
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

        XDMA_LOG("configure: effective ch=%u input=%u %ux%u fps=%u/%u fmt=%u frame_bytes=%zu",
                 stream_desc_.channel_index,
                 static_cast<unsigned>(stream_desc_.input),
                 stream_desc_.width,
                 stream_desc_.height,
                 stream_desc_.fps_num,
                 stream_desc_.fps_den,
                 static_cast<unsigned>(stream_desc_.pixel_format),
                 frame_size_bytes());
        return GV_OK;
    }

    gv_status_t XdmaCaptureSession::start_stream()
    {
        XDMA_LOG("start: begin opened=%d configured=%d running=%d",
                 opened_ ? 1 : 0,
                 configured_ ? 1 : 0,
                 running_ ? 1 : 0);
        if (!opened_ || !configured_)
            return GV_ESTATE;
        if (running_)
            return GV_OK;

        const uint32_t ch = active_channel();
        if (c2h_device_[ch] == INVALID_HANDLE_VALUE)
            return fail(GV_EIO, "c2h handle", ERROR_INVALID_HANDLE);

        if (event_device_ != INVALID_HANDLE_VALUE)
        {
            CloseHandle(event_device_);
            event_device_ = INVALID_HANDLE_VALUE;
        }

        const std::wstring eventName = L"event_" + std::to_wstring(ch * 4);
        event_device_ = open_subdevice(eventName.c_str());
        if (event_device_ == INVALID_HANDLE_VALUE)
            return fail(GV_EIO, "CreateFile(event)");

        const size_t bytes = frame_size_bytes();
        if (bytes == 0)
            return fail(GV_EINVAL, "frame_size_bytes", ERROR_INVALID_PARAMETER);
        XDMA_LOG("start: ch=%u event=%s event_mask=0x%08x capture_reg=0x%lx frame_bytes=%zu",
                 ch,
                 wide_to_utf8(eventName).c_str(),
                 event_mask(),
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

        XDMA_LOG("start: capture disable reg=0x%lx", capture_enable_reg());
        write_user_reg(capture_enable_reg(), 0);
        XDMA_LOG("start: capture enable reg=0x%lx", capture_enable_reg());
        write_user_reg(capture_enable_reg(), 1);
        if (!enable_user_event(event_mask()))
        {
            write_user_reg(capture_enable_reg(), 0);
            return fail(GV_EIO, "IOCTL_XDMA_USER_INT_ENABLE");
        }
        XDMA_LOG("start: user event enabled mask=0x%08x", event_mask());

        running_ = true;
        event_thread_ = std::thread([this]() { event_thread_proc(); });
        data_thread_ = std::thread([this]() { data_thread_proc(); });
        XDMA_LOG("start: threads launched");
        return GV_OK;
    }

    gv_status_t XdmaCaptureSession::stop_stream()
    {
        const bool wasRunning = running_.exchange(false);
        XDMA_LOG("stop: begin was_running=%d", wasRunning ? 1 : 0);
        if (!wasRunning)
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stats_.state = configured_ ? GDRIVER_STREAM_CONFIGURED : GDRIVER_STREAM_STOPPED;
            XDMA_LOG("stop: no active stream state=%u", static_cast<unsigned>(stats_.state));
            return GV_OK;
        }

        XDMA_LOG("stop: disable event mask=0x%08x capture_reg=0x%lx", event_mask(), capture_enable_reg());
        disable_user_event(event_mask());
        write_user_reg(capture_enable_reg(), 0);

        if (event_device_ != INVALID_HANDLE_VALUE)
            CancelIoEx(event_device_, nullptr);
        const uint32_t ch = active_channel();
        if (ch < kMaxChannels && c2h_device_[ch] != INVALID_HANDLE_VALUE)
            CancelIoEx(c2h_device_[ch], nullptr);

        {
            std::lock_guard<std::mutex> lock(mutex_);
            pending_events_ = 0;
        }
        data_cv_.notify_all();
        frame_cv_.notify_all();

        if (event_thread_.joinable())
            event_thread_.join();
        if (data_thread_.joinable())
            data_thread_.join();

        std::lock_guard<std::mutex> lock(mutex_);
        stats_.state = configured_ ? GDRIVER_STREAM_CONFIGURED : GDRIVER_STREAM_STOPPED;
        XDMA_LOG("stop: done captured=%llu delivered=%llu interrupts=%llu dma_errors=%llu",
                 static_cast<unsigned long long>(stats_.frames_captured),
                 static_cast<unsigned long long>(stats_.frames_delivered),
                 static_cast<unsigned long long>(stats_.interrupt_count),
                 static_cast<unsigned long long>(stats_.dma_errors));
        return GV_OK;
    }

    gv_status_t XdmaCaptureSession::wait_frame(uint32_t timeoutMs, gv_frame_t &out)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        std::memset(&out, 0, sizeof(out));
        if (!running_)
            return GV_ESTATE;

        const auto hasFrame = [this]() {
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
            return GV_ETIMEOUT;
        }

        if (!running_ && latest_sequence_ <= delivered_sequence_)
        {
            XDMA_LOG("wait_frame: stopped without pending frame");
            return GV_ESTATE;
        }
        if (stream_error_ && latest_sequence_ <= delivered_sequence_)
        {
            XDMA_LOG("wait_frame: stream error latest=%llu delivered=%llu",
                     static_cast<unsigned long long>(latest_sequence_),
                     static_cast<unsigned long long>(delivered_sequence_));
            return GV_EIO;
        }
        if (latest_sequence_ <= delivered_sequence_)
            return GV_ETIMEOUT;

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
        out.bit_depth = 8;
        out.plane_count = 1;
        out.plane_offset_bytes[0] = 0;
        out.plane_stride_bytes[0] = stream_desc_.width * 2;
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
        return GV_OK;
    }

    gv_status_t XdmaCaptureSession::release_frame(const gv_frame_t &)
    {
        return GV_OK;
    }

    void XdmaCaptureSession::event_thread_proc()
    {
        XDMA_LOG("event_thread: start mask=0x%08x", event_mask());
        while (running_)
        {
            uint8_t value = 0;
            const int ret = read_device(event_device_, 0, 1, &value);
            if (!running_)
                break;
            if (ret < 0)
            {
                fail(GV_EIO, "ReadFile(event)");
                std::lock_guard<std::mutex> lock(mutex_);
                stream_error_ = true;
                ++stats_.dma_errors;
                frame_cv_.notify_all();
                break;
            }

            uint64_t interruptCount = 0;
            uint32_t pending = 0;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                ++pending_events_;
                ++stats_.interrupt_count;
                interruptCount = stats_.interrupt_count;
                pending = pending_events_;
            }
            if (should_log_counter(interruptCount))
                XDMA_LOG("event_thread: irq=%llu value=%u pending=%u",
                         static_cast<unsigned long long>(interruptCount),
                         static_cast<unsigned>(value),
                         pending);
            data_cv_.notify_one();
        }
        XDMA_LOG("event_thread: exit running=%d", running_ ? 1 : 0);
    }

    void XdmaCaptureSession::data_thread_proc()
    {
        XDMA_LOG("data_thread: start ch=%u frame_bytes=%zu", active_channel(), frame_size_bytes());
        uint64_t readCount = 0;
        while (running_)
        {
            uint32_t pendingAfterPop = 0;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                data_cv_.wait(lock, [this]() { return pending_events_ > 0 || !running_; });
                if (!running_)
                    break;
                --pending_events_;
                pendingAfterPop = pending_events_;
            }

            ++readCount;
            if (should_log_counter(readCount))
                XDMA_LOG("data_thread: event consumed read=%llu pending=%u clear_reg=0x%lx mask=0x%08x",
                         static_cast<unsigned long long>(readCount),
                         pendingAfterPop,
                         kInterruptClearReg,
                         event_mask());
            write_user_reg(kInterruptClearReg, event_mask());
            write_user_reg(kInterruptClearReg, 0);
            enable_user_event(event_mask());

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
            if (ret < 0 || static_cast<DWORD>(ret) != bytes)
            {
                fail(GV_EIO, "ReadFile(c2h)");
                std::lock_guard<std::mutex> lock(mutex_);
                stream_error_ = true;
                ++stats_.dma_errors;
                frame_cv_.notify_all();
                break;
            }

            std::lock_guard<std::mutex> lock(mutex_);
            publish_frame(dma_buffer_.data(), static_cast<size_t>(ret));
        }
        XDMA_LOG("data_thread: exit running=%d", running_ ? 1 : 0);
    }

    void XdmaCaptureSession::publish_frame(const uint8_t *data, size_t bytes)
    {
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
        const bool ok = read_device(user_device_, address, sizeof(out), reinterpret_cast<uint8_t *>(&out)) == sizeof(out);
        if (!ok)
            XDMA_LOG("read_user_reg: addr=0x%lx failed", address);
        return ok;
    }

    bool XdmaCaptureSession::write_user_reg(long address, uint32_t value) const
    {
        const bool ok = write_device(user_device_, address, sizeof(value), reinterpret_cast<const uint8_t *>(&value)) == sizeof(value);
        if (!ok)
            XDMA_LOG("write_user_reg: addr=0x%lx value=0x%08x failed", address, value);
        return ok;
    }

    bool XdmaCaptureSession::enable_user_event(uint32_t mask)
    {
        static std::atomic<uint64_t> enableLogCount{0};
        DWORD returned = 0;
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

    gv_status_t XdmaCaptureSession::fail(gv_status_t status, const char *where, DWORD winerr) const
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

    uint32_t XdmaCaptureSession::event_mask() const
    {
        return bit_n(active_channel() * 4);
    }

    long XdmaCaptureSession::capture_enable_reg() const
    {
        return active_channel() == 0 ? kVideo0CaptureEnableReg : kVideo1CaptureEnableReg;
    }

    size_t XdmaCaptureSession::frame_size_bytes() const
    {
        return static_cast<size_t>(stream_desc_.width) * static_cast<size_t>(stream_desc_.height) * 2u;
    }
}
