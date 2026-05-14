#ifdef _WIN32
#include "ks_capture_session.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <cwctype>
#include <limits>
#include <memory>
#include <sstream>
#include <thread>

namespace
{
    static const GUID kSubtypeYuy2 =
        {0x32595559, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
    static const GUID kSubtypeUyvy =
        {0x59565955, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
    static const GUID kSubtypeY210 =
        {0x30313259, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
    static const GUID kSubtypeRgb24 =
        {0xe436eb7d, 0x524f, 0x11ce, {0x9f, 0x53, 0x00, 0x20, 0xaf, 0x0b, 0xa7, 0x70}};

    static gv_status_t bool_to_status(bool ok)
    {
        return ok ? GV_OK : GV_EIO;
    }

    static uint32_t clamp_nonzero(uint32_t value, uint32_t fallback)
    {
        return value ? value : fallback;
    }

    static uint32_t bytes_per_pixel(gdriver_pixel_format_t fmt)
    {
        switch (fmt)
        {
        case GDRIVER_PIXFMT_YUY2:
        case GDRIVER_PIXFMT_UYVY:
            return 2;
        case GDRIVER_PIXFMT_RGB24:
            return 3;
        case GDRIVER_PIXFMT_BGRX32:
            return 4;
        case GDRIVER_PIXFMT_Y210:
            return 4;
        default:
            return 0;
        }
    }

    static bool query_ks_property(HANDLE handle,
                                  void *request,
                                  DWORD requestBytes,
                                  void *output,
                                  DWORD outputBytes,
                                  DWORD *returned = nullptr)
    {
        DWORD bytes = 0;
        const BOOL ok = DeviceIoControl(handle,
                                        IOCTL_KS_PROPERTY,
                                        request,
                                        requestBytes,
                                        output,
                                        outputBytes,
                                        &bytes,
                                        nullptr);
        if (returned)
            *returned = bytes;
        return ok != FALSE;
    }

    static bool query_ks_property_variable(HANDLE handle,
                                           void *request,
                                           DWORD requestBytes,
                                           std::vector<uint8_t> &output)
    {
        output.clear();

        DWORD bytes = 0;
        ULONG sizeQuery = 0;
        if (DeviceIoControl(handle,
                            IOCTL_KS_PROPERTY,
                            request,
                            requestBytes,
                            &sizeQuery,
                            sizeof(sizeQuery),
                            &bytes,
                            nullptr) &&
            sizeQuery > 0)
        {
            output.assign(sizeQuery, 0);
            bytes = 0;
            const BOOL ok = DeviceIoControl(handle,
                                            IOCTL_KS_PROPERTY,
                                            request,
                                            requestBytes,
                                            output.data(),
                                            static_cast<DWORD>(output.size()),
                                            &bytes,
                                            nullptr);
            if (ok)
            {
                if (bytes > 0 && bytes < output.size())
                    output.resize(bytes);
                return true;
            }
        }

        DWORD needed = 4096;

        for (int attempt = 0; attempt < 4; ++attempt)
        {
            output.assign(needed, 0);
            bytes = 0;
            const BOOL ok = DeviceIoControl(handle,
                                            IOCTL_KS_PROPERTY,
                                            request,
                                            requestBytes,
                                            output.data(),
                                            static_cast<DWORD>(output.size()),
                                            &bytes,
                                            nullptr);
            if (ok)
            {
                if (bytes > 0 && bytes < output.size())
                    output.resize(bytes);
                return true;
            }

            const DWORD err = GetLastError();
            if (err != ERROR_MORE_DATA && err != ERROR_INSUFFICIENT_BUFFER)
                break;
            needed *= 2;
        }

        output.clear();
        return false;
    }

    static bool write_connection_state(HANDLE pinHandle, KSSTATE state)
    {
        KSPROPERTY prop = {};
        prop.Set = KSPROPSETID_Connection;
        prop.Id = KSPROPERTY_CONNECTION_STATE;
        prop.Flags = KSPROPERTY_TYPE_SET;
        DWORD bytes = 0;
        return DeviceIoControl(pinHandle,
                               IOCTL_KS_PROPERTY,
                               &prop,
                               sizeof(prop),
                               &state,
                               sizeof(state),
                               &bytes,
                               nullptr) != FALSE;
    }

    static DWORD fourcc(char a, char b, char c, char d)
    {
        return (static_cast<DWORD>(static_cast<unsigned char>(a))) |
               (static_cast<DWORD>(static_cast<unsigned char>(b)) << 8) |
               (static_cast<DWORD>(static_cast<unsigned char>(c)) << 16) |
               (static_cast<DWORD>(static_cast<unsigned char>(d)) << 24);
    }

    static gdriver_pixel_format_t pixfmt_from_subtype(const GUID &subtype)
    {
        if (subtype == kSubtypeYuy2)
            return GDRIVER_PIXFMT_YUY2;
        if (subtype == kSubtypeUyvy)
            return GDRIVER_PIXFMT_UYVY;
        if (subtype == kSubtypeY210)
            return GDRIVER_PIXFMT_Y210;
        if (subtype == kSubtypeRgb24)
            return GDRIVER_PIXFMT_RGB24;
        return GDRIVER_PIXFMT_UNKNOWN;
    }

    static gdriver_pixel_format_t pixfmt_from_bitmap_header(const KS_BITMAPINFOHEADER &bmi)
    {
        if (bmi.biCompression == fourcc('Y', 'U', 'Y', '2') && bmi.biBitCount == 16)
            return GDRIVER_PIXFMT_YUY2;
        if (bmi.biCompression == fourcc('U', 'Y', 'V', 'Y') && bmi.biBitCount == 16)
            return GDRIVER_PIXFMT_UYVY;
        if (bmi.biCompression == fourcc('Y', '2', '1', '0') && bmi.biBitCount == 32)
            return GDRIVER_PIXFMT_Y210;
        if (bmi.biCompression == KS_BI_RGB && bmi.biBitCount == 24)
            return GDRIVER_PIXFMT_RGB24;
        return GDRIVER_PIXFMT_UNKNOWN;
    }

    static std::string guid_to_compact_string(const GUID &g)
    {
        char buf[64] = {};
        sprintf_s(buf, sizeof(buf),
                  "%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X",
                  static_cast<unsigned long>(g.Data1),
                  static_cast<unsigned>(g.Data2),
                  static_cast<unsigned>(g.Data3),
                  static_cast<unsigned>(g.Data4[0]),
                  static_cast<unsigned>(g.Data4[1]),
                  static_cast<unsigned>(g.Data4[2]),
                  static_cast<unsigned>(g.Data4[3]),
                  static_cast<unsigned>(g.Data4[4]),
                  static_cast<unsigned>(g.Data4[5]),
                  static_cast<unsigned>(g.Data4[6]),
                  static_cast<unsigned>(g.Data4[7]));
        return std::string(buf);
    }

    static std::string fourcc_to_string(DWORD value)
    {
        char s[5] = {};
        s[0] = static_cast<char>(value & 0xff);
        s[1] = static_cast<char>((value >> 8) & 0xff);
        s[2] = static_cast<char>((value >> 16) & 0xff);
        s[3] = static_cast<char>((value >> 24) & 0xff);
        for (char &c : s)
        {
            if (c < 32 || c > 126)
                c = '.';
        }
        return std::string(s);
    }

    static bool contains_lower(std::wstring value, const wchar_t *needle)
    {
        std::transform(value.begin(), value.end(), value.begin(),
                       [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
        return value.find(needle) != std::wstring::npos;
    }
}

namespace gvendor
{
    KsCaptureSession::~KsCaptureSession()
    {
        close();
    }

    gv_status_t KsCaptureSession::open_default()
    {
        clear_error();
        devices_ = enumerate_ks_capture_devices();
        if (devices_.empty())
            return fail(GV_ENODEV, "enumerate_ks_capture_devices", ERROR_NOT_FOUND);

        size_t preferred = static_cast<size_t>(-1);
        for (size_t i = 0; i < devices_.size(); ++i)
        {
            if (contains_lower(devices_[i].interface_path, L"64183a34-7256-40ed-b9ff-fafdc69e666c"))
            {
                preferred = i;
                break;
            }
        }
        for (size_t i = 0; i < devices_.size(); ++i)
        {
            if (preferred != static_cast<size_t>(-1))
                break;
            if (device_is_preferred_capture_card(devices_[i]))
            {
                preferred = i;
                break;
            }
        }
        for (size_t i = 0; i < devices_.size(); ++i)
        {
            if (preferred != static_cast<size_t>(-1))
                break;
            if (devices_[i].inferred_input == GDRIVER_INPUT_HDMI)
            {
                preferred = i;
                break;
            }
        }
        if (preferred == static_cast<size_t>(-1))
        {
            for (size_t i = 0; i < devices_.size(); ++i)
            {
                std::wstring name = devices_[i].friendly_name;
                std::transform(name.begin(), name.end(), name.begin(),
                               [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
                if (name.find(L"gigabyte") != std::wstring::npos ||
                    name.find(L"video") != std::wstring::npos)
                {
                    preferred = i;
                    break;
                }
            }
        }
        if (preferred == static_cast<size_t>(-1))
            preferred = 0;

        selected_device_index_ = preferred;
        selected_input_ = devices_[preferred].inferred_input;
        return open_filter(devices_[preferred]);
    }

    gv_status_t KsCaptureSession::open_device_index(size_t deviceIndex)
    {
        clear_error();
        devices_ = enumerate_ks_capture_devices();
        if (devices_.empty())
            return fail(GV_ENODEV, "enumerate_ks_capture_devices", ERROR_NOT_FOUND);
        if (deviceIndex >= devices_.size())
            return fail(GV_EINVAL, "open_device_index", ERROR_INVALID_PARAMETER);

        selected_device_index_ = deviceIndex;
        selected_input_ = devices_[deviceIndex].inferred_input;
        return open_filter(devices_[deviceIndex]);
    }

    gv_status_t KsCaptureSession::close()
    {
        stop_stream();
        close_pin();

        if (filter_handle_ != INVALID_HANDLE_VALUE)
        {
            CloseHandle(filter_handle_);
            filter_handle_ = INVALID_HANDLE_VALUE;
        }

        configured_ = false;
        running_ = false;
        frame_buffer_.clear();
        return GV_OK;
    }

    gv_status_t KsCaptureSession::set_input(gdriver_input_t input, uint32_t)
    {
        clear_error();
        if (configured_ || running_)
            return fail(GV_ESTATE, "set_input while configured/running", ERROR_INVALID_STATE);
        return reopen_for_input(input);
    }

    gv_status_t KsCaptureSession::get_device_info(gv_device_info_t &out) const
    {
        std::memset(&out, 0, sizeof(out));
        if (selected_device_index_ >= devices_.size())
            return GV_ENODEV;

        const KsCaptureDevice &device = devices_[selected_device_index_];
        if (!device.friendly_name.empty())
        {
            WideCharToMultiByte(CP_UTF8, 0, device.friendly_name.c_str(), -1,
                                out.friendly_name, static_cast<int>(sizeof(out.friendly_name)), nullptr, nullptr);
        }

        switch (device.inferred_input)
        {
        case GDRIVER_INPUT_HDMI:
            out.supported_inputs_mask = 1u << GDRIVER_INPUT_HDMI;
            break;
        case GDRIVER_INPUT_SDI:
            out.supported_inputs_mask = 1u << GDRIVER_INPUT_SDI;
            break;
        default:
            out.supported_inputs_mask = 0;
            break;
        }

        out.supported_pixel_formats_mask =
            (1u << GDRIVER_PIXFMT_YUY2) |
            (1u << GDRIVER_PIXFMT_RGB24);
        out.max_video_channels = 1;
        out.max_audio_channels = 0;
        return GV_OK;
    }

    gv_status_t KsCaptureSession::get_signal_status(gv_signal_status_t &out) const
    {
        std::memset(&out, 0, sizeof(out));
        out.input = selected_input_;
        out.width = stream_desc_.width;
        out.height = stream_desc_.height;
        out.fps_num = stream_desc_.fps_num;
        out.fps_den = stream_desc_.fps_den;
        out.pixel_format = stream_desc_.pixel_format;
        out.bit_depth = stream_desc_.pixel_format == GDRIVER_PIXFMT_Y210 ? 10u : 8u;
        out.signal_locked = configured_ ? 1 : 0;
        return GV_OK;
    }

    gv_status_t KsCaptureSession::get_stream_stats(gv_stream_stats_t &out) const
    {
        std::memset(&out, 0, sizeof(out));
        out.state = running_ ? GDRIVER_STREAM_RUNNING :
                    configured_ ? GDRIVER_STREAM_CONFIGURED :
                                  GDRIVER_STREAM_STOPPED;
        out.frames_captured = frame_counter_;
        out.frames_delivered = delivered_frames_;
        return GV_OK;
    }

    gv_status_t KsCaptureSession::configure_stream(const gv_stream_desc_t &desc)
    {
        clear_error();
        if (filter_handle_ == INVALID_HANDLE_VALUE)
            return fail(GV_ENODEV, "configure_stream without filter", ERROR_INVALID_HANDLE);
        if (running_)
            return fail(GV_ESTATE, "configure_stream while running", ERROR_INVALID_STATE);
        if (desc.pixel_format != GDRIVER_PIXFMT_YUY2 &&
            desc.pixel_format != GDRIVER_PIXFMT_RGB24)
            return fail(GV_ENOTSUP, "unsupported pixel format", ERROR_NOT_SUPPORTED);

        stream_desc_ = desc;
        stream_desc_.width = clamp_nonzero(stream_desc_.width, 1920);
        stream_desc_.height = clamp_nonzero(stream_desc_.height, 1080);
        stream_desc_.fps_num = clamp_nonzero(stream_desc_.fps_num, 30000);
        stream_desc_.fps_den = clamp_nonzero(stream_desc_.fps_den, 1001);
        stream_desc_.buffer_count = clamp_nonzero(stream_desc_.buffer_count, 1);
        stream_desc_.memory_kind = GDRIVER_MEMORY_DRIVER_COPY;

        const gv_status_t createStatus = create_pin();
        configured_ = (createStatus == GV_OK);
        return createStatus;
    }

    gv_status_t KsCaptureSession::start_stream()
    {
        clear_error();
        if (!configured_ || pin_handle_ == INVALID_HANDLE_VALUE)
            return fail(GV_ESTATE, "start_stream without configured pin", ERROR_INVALID_STATE);
        if (running_)
            return GV_OK;

        const std::array<KSSTATE, 3> states = {KSSTATE_ACQUIRE, KSSTATE_PAUSE, KSSTATE_RUN};
        for (KSSTATE state : states)
        {
            const gv_status_t st = set_pin_state(state);
            if (st != GV_OK)
                return st;
        }
        running_ = true;
        return GV_OK;
    }

    gv_status_t KsCaptureSession::stop_stream()
    {
        if (pin_handle_ == INVALID_HANDLE_VALUE)
        {
            running_ = false;
            return GV_OK;
        }

        if (running_)
        {
            const std::array<KSSTATE, 3> states = {KSSTATE_PAUSE, KSSTATE_ACQUIRE, KSSTATE_STOP};
            for (KSSTATE state : states)
            {
                const gv_status_t st = set_pin_state(state);
                if (st != GV_OK)
                    return st;
            }
        }

        running_ = false;
        return GV_OK;
    }

    gv_status_t KsCaptureSession::wait_frame(uint32_t timeoutMs, gv_frame_t &out)
    {
        clear_error();
        std::memset(&out, 0, sizeof(out));
        if (!running_ || pin_handle_ == INVALID_HANDLE_VALUE || frame_buffer_.empty())
            return fail(GV_ESTATE, "wait_frame without running stream", ERROR_INVALID_STATE);

        struct StreamPacket
        {
            KSSTREAM_HEADER header;
            KS_FRAME_INFO frameInfo;
        };

        auto packet = std::make_shared<StreamPacket>();
        auto readBuffer = std::make_shared<std::vector<uint8_t>>(frame_buffer_.size(), 0);
        packet->header.Size = sizeof(StreamPacket);
        packet->header.FrameExtent = static_cast<ULONG>(readBuffer->size());
        packet->header.DataUsed = 0;
        packet->header.Data = readBuffer->data();

        struct ReadResult
        {
            BOOL ok = FALSE;
            DWORD bytes = 0;
            DWORD winerr = ERROR_SUCCESS;
        };

        auto result = std::make_shared<ReadResult>();
        std::thread reader([packet, readBuffer, result, pin = pin_handle_]() {
            result->ok = DeviceIoControl(pin,
                                         IOCTL_KS_READ_STREAM,
                                         nullptr,
                                         0,
                                         packet.get(),
                                         sizeof(StreamPacket),
                                         &result->bytes,
                                         nullptr);
            result->winerr = result->ok ? ERROR_SUCCESS : GetLastError();
        });

        const DWORD waitMs = timeoutMs == 0 ? INFINITE : timeoutMs;
        const DWORD waitResult = WaitForSingleObject(reader.native_handle(), waitMs);
        bool joined = false;
        if (waitResult == WAIT_TIMEOUT)
        {
            CancelSynchronousIo(reader.native_handle());
            if (WaitForSingleObject(reader.native_handle(), 2000) == WAIT_OBJECT_0)
            {
                reader.join();
                joined = true;
            }
            else
            {
                reader.detach();
            }
        }
        if (!joined && waitResult != WAIT_TIMEOUT)
            reader.join();

        if (waitResult == WAIT_TIMEOUT)
        {
            std::ostringstream oss;
            oss << "IOCTL_KS_READ_STREAM timed out"
                << " bytes_returned=" << result->bytes
                << " data_used=" << packet->header.DataUsed
                << " cancel_winerr=" << result->winerr
                << " cancel_completed=" << (joined ? 1 : 0);
            last_error_ = oss.str();
            return GV_ETIMEOUT;
        }

        if (!result->ok)
            return fail(GV_EIO, "IOCTL_KS_READ_STREAM", result->winerr);

        if (packet->header.DataUsed > 0)
        {
            ++frame_counter_;
            ++delivered_frames_;
            frame_buffer_.assign(readBuffer->begin(), readBuffer->end());

            out.data = frame_buffer_.data();
            out.data_size_bytes = packet->header.DataUsed;
            out.frame_id = frame_counter_;
            out.timestamp_ns = 0;
            out.width = stream_desc_.width;
            out.height = stream_desc_.height;
            out.pixel_format = stream_desc_.pixel_format;
            out.bit_depth = stream_desc_.pixel_format == GDRIVER_PIXFMT_Y210 ? 10 : 8;
            out.plane_count = 1;
            out.plane_offset_bytes[0] = 0;
            out.plane_stride_bytes[0] = stream_desc_.width * bytes_per_pixel(stream_desc_.pixel_format);
            out.driver_buffer_index = packet->frameInfo.FrameCompletionNumber;
            return GV_OK;
        }

        std::ostringstream oss;
        oss << "IOCTL_KS_READ_STREAM returned no data"
            << " bytes_returned=" << result->bytes
            << " data_used=" << packet->header.DataUsed
            << " winerr=" << result->winerr;
        last_error_ = oss.str();
        return GV_ETIMEOUT;
    }

    gv_status_t KsCaptureSession::release_frame(const gv_frame_t &)
    {
        return GV_OK;
    }

    const char *KsCaptureSession::last_error() const
    {
        return last_error_.c_str();
    }

    gv_status_t KsCaptureSession::fail(gv_status_t status, const char *where, DWORD winerr) const
    {
        std::ostringstream oss;
        oss << (where ? where : "operation") << " failed";
        if (winerr != ERROR_SUCCESS)
            oss << " winerr=" << winerr;
        last_error_ = oss.str();
        return status;
    }

    void KsCaptureSession::clear_error() const
    {
        last_error_.clear();
    }

    gv_status_t KsCaptureSession::reopen_for_input(gdriver_input_t input)
    {
        if (devices_.empty())
            devices_ = enumerate_ks_capture_devices();
        if (devices_.empty())
            return fail(GV_ENODEV, "enumerate_ks_capture_devices", ERROR_NOT_FOUND);

        for (size_t i = 0; i < devices_.size(); ++i)
        {
            if (!device_matches_input(devices_[i], input))
                continue;

            if (filter_handle_ != INVALID_HANDLE_VALUE)
            {
                CloseHandle(filter_handle_);
                filter_handle_ = INVALID_HANDLE_VALUE;
            }

            selected_device_index_ = i;
            selected_input_ = devices_[i].inferred_input;
            return open_filter(devices_[i]);
        }

        // Many AVStream devices expose one KS interface per filter without a
        // reliable HDMI/SDI string in the friendly name. For v0.1, keep the
        // already-opened filter and treat input as a logical preference so we
        // can validate the direct KS streaming path before driver-specific
        // routing exists.
        if (filter_handle_ != INVALID_HANDLE_VALUE && selected_device_index_ < devices_.size())
        {
            selected_input_ = input;
            return GV_OK;
        }

        return fail(GV_ENODEV, "reopen_for_input", ERROR_NOT_FOUND);
    }

    gv_status_t KsCaptureSession::open_filter(const KsCaptureDevice &device)
    {
        filter_handle_ = CreateFileW(device.interface_path.c_str(),
                                     GENERIC_READ | GENERIC_WRITE,
                                     FILE_SHARE_READ | FILE_SHARE_WRITE,
                                     nullptr,
                                     OPEN_EXISTING,
                                     FILE_ATTRIBUTE_NORMAL,
                                     nullptr);
        return filter_handle_ == INVALID_HANDLE_VALUE ? fail(GV_EIO, "CreateFileW(filter)") : GV_OK;
    }

    gv_status_t KsCaptureSession::close_pin()
    {
        if (pin_handle_ != INVALID_HANDLE_VALUE)
        {
            CloseHandle(pin_handle_);
            pin_handle_ = INVALID_HANDLE_VALUE;
        }
        return GV_OK;
    }

    gv_status_t KsCaptureSession::create_pin()
    {
        close_pin();

        uint32_t pinCount = 0;
        gv_status_t st = query_pin_count(pinCount);
        if (st != GV_OK)
            return st;

        std::vector<gv_stream_desc_t> candidates;
        candidates.push_back(stream_desc_);

        auto add_candidate = [&](uint32_t w, uint32_t h, gdriver_pixel_format_t fmt)
        {
            for (const auto &existing : candidates)
            {
                if (existing.width == w && existing.height == h && existing.pixel_format == fmt)
                    return;
            }
            gv_stream_desc_t c = stream_desc_;
            c.width = w;
            c.height = h;
            c.fps_num = 30000;
            c.fps_den = 1001;
            c.pixel_format = fmt;
            candidates.push_back(c);
        };

        add_candidate(1920, 1080, GDRIVER_PIXFMT_YUY2);
        add_candidate(1280, 720, GDRIVER_PIXFMT_YUY2);
        add_candidate(1920, 1080, GDRIVER_PIXFMT_UYVY);
        add_candidate(1280, 720, GDRIVER_PIXFMT_UYVY);
        add_candidate(1920, 1080, GDRIVER_PIXFMT_Y210);
        add_candidate(1920, 1080, GDRIVER_PIXFMT_RGB24);

        DWORD lastWinerr = ERROR_NO_MATCH;
        uint32_t tried = 0;
        for (uint32_t pin = 0; pin < pinCount; ++pin)
        {
            KSPIN_DATAFLOW flow = KSPIN_DATAFLOW_IN;
            if (query_pin_dataflow(pin, flow) != GV_OK || flow != KSPIN_DATAFLOW_OUT)
                continue;

            std::vector<uint8_t> ranges;
            if (query_pin_data_ranges(pin, ranges) == GV_OK && ranges.size() >= sizeof(KSMULTIPLE_ITEM))
            {
                const auto *multi = reinterpret_cast<const KSMULTIPLE_ITEM *>(ranges.data());
                size_t offset = sizeof(KSMULTIPLE_ITEM);
                for (ULONG i = 0; i < multi->Count && offset + sizeof(KSDATARANGE) <= ranges.size(); ++i)
                {
                    const auto *range = reinterpret_cast<const KSDATARANGE *>(ranges.data() + offset);
                    if (range->FormatSize < sizeof(KSDATARANGE) || offset + range->FormatSize > ranges.size())
                        break;

                    std::vector<uint8_t> format;
                    const gv_status_t intersectStatus = query_pin_data_intersection(pin, range, format);
                    if (intersectStatus == GV_OK && format.size() >= sizeof(KSDATAFORMAT))
                    {
                        ++tried;
                        const gv_status_t tryStatus = try_create_pin_with_format(pin, format.data(), format.size());
                        if (tryStatus == GV_OK)
                            return GV_OK;
                        lastWinerr = GetLastError();
                    }
                    else
                    {
                        lastWinerr = GetLastError();
                    }

                    offset += range->FormatSize;
                    offset = (offset + 7u) & ~size_t(7u);
                }
            }

            add_data_range_candidates(pin, candidates);

            for (const auto &candidate : candidates)
            {
                ++tried;
                const gv_status_t tryStatus = try_create_pin(pin, candidate);
                if (tryStatus == GV_OK)
                    return GV_OK;
                lastWinerr = GetLastError();
            }
        }

        std::ostringstream oss;
        oss << "KsCreatePin no matching output pin/format tried=" << tried
            << " last_winerr=" << lastWinerr
            << " notes=" << last_error_;
        last_error_ = oss.str();
        return GV_EIO;
    }

    gv_status_t KsCaptureSession::try_create_pin(uint32_t pinId, const gv_stream_desc_t &desc)
    {
        KS_DATAFORMAT_VIDEOINFOHEADER format = {};
        size_t frameBytes = 0;
        gv_status_t st = build_format(desc, format, frameBytes);
        if (st != GV_OK)
            return st;

        std::vector<uint8_t> connectBytes(sizeof(KSPIN_CONNECT) + sizeof(format), 0);
        auto *connect = reinterpret_cast<KSPIN_CONNECT *>(connectBytes.data());
        connect->Interface.Set = KSINTERFACESETID_Standard;
        connect->Interface.Id = KSINTERFACE_STANDARD_STREAMING;
        connect->Medium.Set = KSMEDIUMSETID_Standard;
        connect->Medium.Id = KSMEDIUM_STANDARD_DEVIO;
        connect->PinId = pinId;
        connect->PinToHandle = nullptr;
        connect->Priority.PriorityClass = KSPRIORITY_NORMAL;
        connect->Priority.PrioritySubClass = 1;
        std::memcpy(connectBytes.data() + sizeof(KSPIN_CONNECT), &format, sizeof(format));

        HANDLE pin = INVALID_HANDLE_VALUE;
        const DWORD rc = KsCreatePin(filter_handle_, connect, GENERIC_READ, &pin);
        if (rc != ERROR_SUCCESS || pin == INVALID_HANDLE_VALUE)
        {
            SetLastError(rc != ERROR_SUCCESS ? rc : GetLastError());
            return GV_EIO;
        }

        pin_handle_ = pin;
        selected_pin_id_ = pinId;
        stream_desc_ = desc;
        frame_buffer_.assign(frameBytes, 0);
        return GV_OK;
    }

    gv_status_t KsCaptureSession::try_create_pin_with_format(uint32_t pinId, const void *format, size_t formatBytes)
    {
        if (!format || formatBytes < sizeof(KSDATAFORMAT))
            return fail(GV_EINVAL, "try_create_pin_with_format invalid format", ERROR_INVALID_PARAMETER);

        const auto *dataFormat = reinterpret_cast<const KSDATAFORMAT *>(format);
        size_t copyBytes = dataFormat->FormatSize;
        if (copyBytes < sizeof(KSDATAFORMAT) || copyBytes > formatBytes)
            copyBytes = formatBytes;

        std::vector<uint8_t> connectBytes(sizeof(KSPIN_CONNECT) + copyBytes, 0);
        auto *connect = reinterpret_cast<KSPIN_CONNECT *>(connectBytes.data());
        connect->Interface.Set = KSINTERFACESETID_Standard;
        connect->Interface.Id = KSINTERFACE_STANDARD_STREAMING;
        connect->Medium.Set = KSMEDIUMSETID_Standard;
        connect->Medium.Id = KSMEDIUM_STANDARD_DEVIO;
        connect->PinId = pinId;
        connect->PinToHandle = nullptr;
        connect->Priority.PriorityClass = KSPRIORITY_NORMAL;
        connect->Priority.PrioritySubClass = 1;
        std::memcpy(connectBytes.data() + sizeof(KSPIN_CONNECT), format, copyBytes);

        HANDLE pin = INVALID_HANDLE_VALUE;
        const DWORD rc = KsCreatePin(filter_handle_, connect, GENERIC_READ, &pin);
        if (rc != ERROR_SUCCESS || pin == INVALID_HANDLE_VALUE)
        {
            SetLastError(rc != ERROR_SUCCESS ? rc : GetLastError());
            return GV_EIO;
        }

        pin_handle_ = pin;
        selected_pin_id_ = pinId;

        size_t frameBytes = dataFormat->SampleSize;
        if (copyBytes >= sizeof(KS_DATAFORMAT_VIDEOINFOHEADER) &&
            dataFormat->Specifier == KSDATAFORMAT_SPECIFIER_VIDEOINFO)
        {
            const auto *video = reinterpret_cast<const KS_DATAFORMAT_VIDEOINFOHEADER *>(format);
            const KS_BITMAPINFOHEADER &bmi = video->VideoInfoHeader.bmiHeader;
            const uint32_t width = static_cast<uint32_t>(bmi.biWidth > 0 ? bmi.biWidth : -bmi.biWidth);
            const uint32_t height = static_cast<uint32_t>(bmi.biHeight > 0 ? bmi.biHeight : -bmi.biHeight);
            const gdriver_pixel_format_t fmt = pixfmt_from_bitmap_header(bmi) != GDRIVER_PIXFMT_UNKNOWN
                                                   ? pixfmt_from_bitmap_header(bmi)
                                                   : pixfmt_from_subtype(dataFormat->SubFormat);

            if (width > 0)
                stream_desc_.width = width;
            if (height > 0)
                stream_desc_.height = height;
            if (fmt != GDRIVER_PIXFMT_UNKNOWN)
                stream_desc_.pixel_format = fmt;
            if (video->VideoInfoHeader.AvgTimePerFrame > 0)
            {
                stream_desc_.fps_num = 10000000u;
                stream_desc_.fps_den = static_cast<uint32_t>(video->VideoInfoHeader.AvgTimePerFrame);
            }
            if (video->VideoInfoHeader.bmiHeader.biSizeImage > 0)
                frameBytes = video->VideoInfoHeader.bmiHeader.biSizeImage;
        }

        if (frameBytes == 0)
        {
            const uint32_t bpp = bytes_per_pixel(stream_desc_.pixel_format);
            frameBytes = static_cast<size_t>(stream_desc_.width) *
                         static_cast<size_t>(stream_desc_.height) *
                         static_cast<size_t>(bpp);
        }
        if (frameBytes == 0)
            frameBytes = static_cast<size_t>(1920) * 1080 * 2;

        frame_buffer_.assign(frameBytes, 0);
        return GV_OK;
    }

    gv_status_t KsCaptureSession::set_pin_state(KSSTATE state)
    {
        if (pin_handle_ == INVALID_HANDLE_VALUE)
            return fail(GV_ESTATE, "set_pin_state without pin", ERROR_INVALID_HANDLE);
        if (!write_connection_state(pin_handle_, state))
            return fail(GV_EIO, "KSPROPERTY_CONNECTION_STATE");
        return GV_OK;
    }

    gv_status_t KsCaptureSession::query_pin_count(uint32_t &outCount) const
    {
        outCount = 0;
        if (filter_handle_ == INVALID_HANDLE_VALUE)
            return fail(GV_ENODEV, "query_pin_count without filter", ERROR_INVALID_HANDLE);

        KSPROPERTY prop = {};
        prop.Set = KSPROPSETID_Pin;
        prop.Id = KSPROPERTY_PIN_CTYPES;
        prop.Flags = KSPROPERTY_TYPE_GET;

        ULONG count = 0;
        if (!query_ks_property(filter_handle_, &prop, sizeof(prop), &count, sizeof(count)))
            return fail(GV_EIO, "KSPROPERTY_PIN_CTYPES");
        outCount = count;
        return GV_OK;
    }

    gv_status_t KsCaptureSession::query_pin_dataflow(uint32_t pinId, KSPIN_DATAFLOW &outFlow) const
    {
        outFlow = KSPIN_DATAFLOW_IN;
        if (filter_handle_ == INVALID_HANDLE_VALUE)
            return fail(GV_ENODEV, "query_pin_dataflow without filter", ERROR_INVALID_HANDLE);

        KSP_PIN prop = {};
        prop.Property.Set = KSPROPSETID_Pin;
        prop.Property.Id = KSPROPERTY_PIN_DATAFLOW;
        prop.Property.Flags = KSPROPERTY_TYPE_GET;
        prop.PinId = pinId;

        if (!query_ks_property(filter_handle_, &prop, sizeof(prop), &outFlow, sizeof(outFlow)))
            return fail(GV_EIO, "KSPROPERTY_PIN_DATAFLOW");
        return GV_OK;
    }

    gv_status_t KsCaptureSession::query_pin_data_ranges(uint32_t pinId, std::vector<uint8_t> &outRanges) const
    {
        if (filter_handle_ == INVALID_HANDLE_VALUE)
            return fail(GV_ENODEV, "query_pin_data_ranges without filter", ERROR_INVALID_HANDLE);

        KSP_PIN prop = {};
        prop.Property.Set = KSPROPSETID_Pin;
        prop.Property.Id = KSPROPERTY_PIN_DATARANGES;
        prop.Property.Flags = KSPROPERTY_TYPE_GET;
        prop.PinId = pinId;

        if (!query_ks_property_variable(filter_handle_, &prop, sizeof(prop), outRanges))
            return fail(GV_EIO, "KSPROPERTY_PIN_DATARANGES");
        return GV_OK;
    }

    gv_status_t KsCaptureSession::query_pin_data_intersection(uint32_t pinId, const KSDATARANGE *range, std::vector<uint8_t> &outFormat) const
    {
        outFormat.clear();
        if (filter_handle_ == INVALID_HANDLE_VALUE)
            return fail(GV_ENODEV, "query_pin_data_intersection without filter", ERROR_INVALID_HANDLE);
        if (!range || range->FormatSize < sizeof(KSDATARANGE))
            return fail(GV_EINVAL, "query_pin_data_intersection invalid range", ERROR_INVALID_PARAMETER);

        const size_t inputBytes = sizeof(KSP_PIN) + sizeof(KSMULTIPLE_ITEM) + range->FormatSize;
        if (inputBytes > (std::numeric_limits<DWORD>::max)())
            return fail(GV_EINVAL, "query_pin_data_intersection input too large", ERROR_INVALID_PARAMETER);

        std::vector<uint8_t> input(inputBytes, 0);
        auto *prop = reinterpret_cast<KSP_PIN *>(input.data());
        prop->Property.Set = KSPROPSETID_Pin;
        prop->Property.Id = KSPROPERTY_PIN_DATAINTERSECTION;
        prop->Property.Flags = KSPROPERTY_TYPE_GET;
        prop->PinId = pinId;

        auto *multi = reinterpret_cast<KSMULTIPLE_ITEM *>(input.data() + sizeof(KSP_PIN));
        multi->Size = static_cast<ULONG>(sizeof(KSMULTIPLE_ITEM) + range->FormatSize);
        multi->Count = 1;
        std::memcpy(input.data() + sizeof(KSP_PIN) + sizeof(KSMULTIPLE_ITEM), range, range->FormatSize);

        DWORD lastErr = ERROR_SUCCESS;
        const std::array<DWORD, 4> attempts = {
            static_cast<DWORD>(sizeof(ULONG)),
            static_cast<DWORD>(sizeof(KS_DATAFORMAT_VIDEOINFOHEADER)),
            1024u,
            4096u};

        for (DWORD outputBytes : attempts)
        {
            outFormat.assign(outputBytes, 0);
            DWORD returned = 0;
            const BOOL ok = DeviceIoControl(filter_handle_,
                                            IOCTL_KS_PROPERTY,
                                            input.data(),
                                            static_cast<DWORD>(input.size()),
                                            outFormat.data(),
                                            static_cast<DWORD>(outFormat.size()),
                                            &returned,
                                            nullptr);
            if (ok)
            {
                if (returned == sizeof(ULONG) && outputBytes == sizeof(ULONG))
                {
                    const ULONG required = *reinterpret_cast<const ULONG *>(outFormat.data());
                    if (required >= sizeof(KSDATAFORMAT))
                    {
                        outFormat.assign(required, 0);
                        returned = 0;
                        const BOOL retryOk = DeviceIoControl(filter_handle_,
                                                             IOCTL_KS_PROPERTY,
                                                             input.data(),
                                                             static_cast<DWORD>(input.size()),
                                                             outFormat.data(),
                                                             static_cast<DWORD>(outFormat.size()),
                                                             &returned,
                                                             nullptr);
                        if (retryOk && returned >= sizeof(KSDATAFORMAT))
                        {
                            if (returned < outFormat.size())
                                outFormat.resize(returned);
                            return GV_OK;
                        }
                        lastErr = GetLastError();
                    }
                }
                else if (returned >= sizeof(KSDATAFORMAT))
                {
                    if (returned < outFormat.size())
                        outFormat.resize(returned);
                    return GV_OK;
                }
            }
            else
            {
                lastErr = GetLastError();
                if (lastErr != ERROR_MORE_DATA && lastErr != ERROR_INSUFFICIENT_BUFFER)
                    break;
            }
        }

        outFormat.clear();
        return fail(GV_EIO, "KSPROPERTY_PIN_DATAINTERSECTION", lastErr);
    }

    size_t KsCaptureSession::add_data_range_candidates(uint32_t pinId, std::vector<gv_stream_desc_t> &candidates) const
    {
        const size_t before = candidates.size();
        std::vector<uint8_t> ranges;
        if (query_pin_data_ranges(pinId, ranges) != GV_OK || ranges.size() < sizeof(KSMULTIPLE_ITEM))
            return 0;

        const auto *multi = reinterpret_cast<const KSMULTIPLE_ITEM *>(ranges.data());
        std::ostringstream diag;
        diag << "pin " << pinId << " dataranges bytes=" << ranges.size()
             << " count=" << multi->Count;

        size_t offset = sizeof(KSMULTIPLE_ITEM);
        for (ULONG i = 0; i < multi->Count && offset + sizeof(KSDATARANGE) <= ranges.size(); ++i)
        {
            const auto *range = reinterpret_cast<const KSDATARANGE *>(ranges.data() + offset);
            if (range->FormatSize < sizeof(KSDATARANGE) || offset + range->FormatSize > ranges.size())
            {
                diag << " malformed_range_index=" << i << " format_size=" << range->FormatSize;
                break;
            }

            diag << " range[" << i << "] size=" << range->FormatSize;

            if (range->MajorFormat == KSDATAFORMAT_TYPE_VIDEO &&
                range->Specifier == KSDATAFORMAT_SPECIFIER_VIDEOINFO &&
                range->FormatSize >= sizeof(KS_DATARANGE_VIDEO))
            {
                const auto *video = reinterpret_cast<const KS_DATARANGE_VIDEO *>(range);
                gdriver_pixel_format_t fmt = pixfmt_from_subtype(video->DataRange.SubFormat);
                if (fmt == GDRIVER_PIXFMT_UNKNOWN)
                    fmt = pixfmt_from_bitmap_header(video->VideoInfoHeader.bmiHeader);
                diag << " subtype=" << guid_to_compact_string(video->DataRange.SubFormat)
                     << " compression=" << fourcc_to_string(static_cast<DWORD>(video->VideoInfoHeader.bmiHeader.biCompression))
                     << " bitcount=" << video->VideoInfoHeader.bmiHeader.biBitCount
                     << " w=" << video->VideoInfoHeader.bmiHeader.biWidth
                     << " h=" << video->VideoInfoHeader.bmiHeader.biHeight
                     << " fmt=" << static_cast<int>(fmt);
                if (fmt != GDRIVER_PIXFMT_UNKNOWN)
                {
                    gv_stream_desc_t c = stream_desc_;
                    c.width = static_cast<uint32_t>(video->VideoInfoHeader.bmiHeader.biWidth > 0 ? video->VideoInfoHeader.bmiHeader.biWidth : video->ConfigCaps.MaxOutputSize.cx);
                    c.height = static_cast<uint32_t>(video->VideoInfoHeader.bmiHeader.biHeight > 0 ? video->VideoInfoHeader.bmiHeader.biHeight : video->ConfigCaps.MaxOutputSize.cy);
                    c.fps_num = video->VideoInfoHeader.AvgTimePerFrame > 0 ? 10000000u : 30000u;
                    c.fps_den = video->VideoInfoHeader.AvgTimePerFrame > 0 ? static_cast<uint32_t>(video->VideoInfoHeader.AvgTimePerFrame) : 1001u;
                    c.pixel_format = fmt;
                    if (c.width > 0 && c.height > 0)
                    {
                        bool exists = false;
                        for (const auto &existing : candidates)
                        {
                            if (existing.width == c.width && existing.height == c.height &&
                                existing.fps_num == c.fps_num && existing.fps_den == c.fps_den &&
                                existing.pixel_format == c.pixel_format)
                            {
                                exists = true;
                                break;
                            }
                        }
                        if (!exists)
                            candidates.push_back(c);
                    }
                }
            }

            offset += range->FormatSize;
            offset = (offset + 7u) & ~size_t(7u);
        }

        diag << " added=" << (candidates.size() - before);
        last_error_ = diag.str();
        return candidates.size() - before;
    }

    gv_status_t KsCaptureSession::build_format(const gv_stream_desc_t &desc, KS_DATAFORMAT_VIDEOINFOHEADER &outFormat, size_t &outFrameBytes) const
    {
        std::memset(&outFormat, 0, sizeof(outFormat));
        outFrameBytes = 0;

        const uint32_t bpp = bytes_per_pixel(desc.pixel_format);
        if (bpp == 0)
            return fail(GV_ENOTSUP, "build_format unsupported bpp", ERROR_NOT_SUPPORTED);

        const uint64_t frameBytes64 =
            static_cast<uint64_t>(desc.width) *
            static_cast<uint64_t>(desc.height) *
            static_cast<uint64_t>(bpp);
        if (frameBytes64 == 0 || frameBytes64 > static_cast<uint64_t>((std::numeric_limits<ULONG>::max)()))
            return fail(GV_EINVAL, "build_format invalid frame size", ERROR_INVALID_PARAMETER);
        outFrameBytes = static_cast<size_t>(frameBytes64);

        outFormat.DataFormat.FormatSize = sizeof(outFormat);
        outFormat.DataFormat.Flags = 0;
        outFormat.DataFormat.SampleSize = static_cast<ULONG>(outFrameBytes);
        outFormat.DataFormat.Reserved = 0;
        outFormat.DataFormat.MajorFormat = KSDATAFORMAT_TYPE_VIDEO;
        if (desc.pixel_format == GDRIVER_PIXFMT_RGB24)
            outFormat.DataFormat.SubFormat = kSubtypeRgb24;
        else if (desc.pixel_format == GDRIVER_PIXFMT_UYVY)
            outFormat.DataFormat.SubFormat = kSubtypeUyvy;
        else if (desc.pixel_format == GDRIVER_PIXFMT_Y210)
            outFormat.DataFormat.SubFormat = kSubtypeY210;
        else
            outFormat.DataFormat.SubFormat = kSubtypeYuy2;
        outFormat.DataFormat.Specifier = KSDATAFORMAT_SPECIFIER_VIDEOINFO;

        KS_VIDEOINFOHEADER &vih = outFormat.VideoInfoHeader;
        vih.AvgTimePerFrame = desc.fps_num
                                  ? static_cast<LONGLONG>((10000000ULL * desc.fps_den) / desc.fps_num)
                                  : 333667;
        vih.bmiHeader.biSize = sizeof(KS_BITMAPINFOHEADER);
        vih.bmiHeader.biWidth = static_cast<LONG>(desc.width);
        vih.bmiHeader.biHeight = static_cast<LONG>(desc.height);
        vih.bmiHeader.biPlanes = 1;
        vih.bmiHeader.biBitCount = static_cast<WORD>(bpp * 8u);
        if (desc.pixel_format == GDRIVER_PIXFMT_RGB24)
            vih.bmiHeader.biCompression = KS_BI_RGB;
        else if (desc.pixel_format == GDRIVER_PIXFMT_UYVY)
            vih.bmiHeader.biCompression = fourcc('U', 'Y', 'V', 'Y');
        else if (desc.pixel_format == GDRIVER_PIXFMT_Y210)
            vih.bmiHeader.biCompression = fourcc('Y', '2', '1', '0');
        else
            vih.bmiHeader.biCompression = fourcc('Y', 'U', 'Y', '2');
        vih.bmiHeader.biSizeImage = static_cast<ULONG>(outFrameBytes);
        return GV_OK;
    }
}
#endif
