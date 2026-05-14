#pragma once

#ifdef _WIN32
#include <windows.h>
#include <ks.h>
#include <ksmedia.h>
#include <string>
#include <vector>
#include "gvendor.h"
#include "ks_device_enumerator.h"

namespace gvendor
{
    class KsCaptureSession
    {
    public:
        KsCaptureSession() = default;
        ~KsCaptureSession();

        gv_status_t open_default();
        gv_status_t open_device_index(size_t deviceIndex);
        gv_status_t close();

        gv_status_t set_input(gdriver_input_t input, uint32_t channelIndex);
        gv_status_t get_device_info(gv_device_info_t &out) const;
        gv_status_t get_signal_status(gv_signal_status_t &out) const;
        gv_status_t get_stream_stats(gv_stream_stats_t &out) const;

        gv_status_t configure_stream(const gv_stream_desc_t &desc);
        gv_status_t start_stream();
        gv_status_t stop_stream();
        gv_status_t wait_frame(uint32_t timeoutMs, gv_frame_t &out);
        gv_status_t release_frame(const gv_frame_t &frame);
        const char *last_error() const;

    private:
        gv_status_t fail(gv_status_t status, const char *where, DWORD winerr = GetLastError()) const;
        void clear_error() const;
        gv_status_t reopen_for_input(gdriver_input_t input);
        gv_status_t open_filter(const KsCaptureDevice &device);
        gv_status_t close_pin();
        gv_status_t create_pin();
        gv_status_t try_create_pin(uint32_t pinId, const gv_stream_desc_t &desc);
        gv_status_t try_create_pin_with_format(uint32_t pinId, const void *format, size_t formatBytes);
        gv_status_t set_pin_state(KSSTATE state);
        gv_status_t query_pin_count(uint32_t &outCount) const;
        gv_status_t query_pin_dataflow(uint32_t pinId, KSPIN_DATAFLOW &outFlow) const;
        gv_status_t query_pin_data_ranges(uint32_t pinId, std::vector<uint8_t> &outRanges) const;
        gv_status_t query_pin_data_intersection(uint32_t pinId, const KSDATARANGE *range, std::vector<uint8_t> &outFormat) const;
        size_t add_data_range_candidates(uint32_t pinId, std::vector<gv_stream_desc_t> &candidates) const;
        gv_status_t build_format(const gv_stream_desc_t &desc, KS_DATAFORMAT_VIDEOINFOHEADER &outFormat, size_t &outFrameBytes) const;

        HANDLE filter_handle_ = INVALID_HANDLE_VALUE;
        HANDLE pin_handle_ = INVALID_HANDLE_VALUE;
        std::vector<KsCaptureDevice> devices_;
        size_t selected_device_index_ = static_cast<size_t>(-1);
        uint32_t selected_pin_id_ = 0;
        gdriver_input_t selected_input_ = GDRIVER_INPUT_UNKNOWN;
        gv_stream_desc_t stream_desc_{};
        bool configured_ = false;
        bool running_ = false;
        std::vector<uint8_t> frame_buffer_;
        uint64_t frame_counter_ = 0;
        uint64_t delivered_frames_ = 0;
        mutable std::string last_error_;
    };
}
#endif
