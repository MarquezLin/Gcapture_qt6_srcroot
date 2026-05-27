#pragma once

#include "gvendor.h"

#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace gvendor
{
    struct XdmaDevice
    {
        std::wstring interface_path;
        std::wstring friendly_name;
    };

    std::vector<XdmaDevice> enumerate_xdma_devices();

    class XdmaCaptureSession
    {
    public:
        XdmaCaptureSession();
        XdmaCaptureSession(const XdmaCaptureSession &) = delete;
        XdmaCaptureSession &operator=(const XdmaCaptureSession &) = delete;
        ~XdmaCaptureSession();

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
        gv_status_t open_device(const XdmaDevice &device);
        void close_handles();

        HANDLE open_subdevice(const wchar_t *name) const;
        int read_device(HANDLE device, long address, DWORD size, uint8_t *buffer) const;
        int write_device(HANDLE device, long address, DWORD size, const uint8_t *buffer) const;
        bool read_user_reg(long address, uint32_t &out) const;
        bool write_user_reg(long address, uint32_t value) const;
        bool enable_user_event(uint32_t mask);
        bool disable_user_event(uint32_t mask);

        void event_thread_proc();
        void data_thread_proc();
        void publish_frame(const uint8_t *data, size_t bytes);

        gv_status_t fail(gv_status_t status, const char *where, DWORD winerr = GetLastError()) const;
        void set_last_error(const std::string &message) const;
        void clear_last_error() const;

        uint32_t active_channel() const;
        uint32_t event_mask() const;
        long capture_enable_reg() const;
        size_t frame_size_bytes() const;

        std::wstring base_path_;
        std::wstring friendly_name_;

        HANDLE user_device_ = INVALID_HANDLE_VALUE;
        HANDLE c2h_device_[2] = {INVALID_HANDLE_VALUE, INVALID_HANDLE_VALUE};
        HANDLE event_device_ = INVALID_HANDLE_VALUE;

        gv_stream_desc_t stream_desc_{};
        gdriver_input_t input_ = GDRIVER_INPUT_SDI;
        bool opened_ = false;
        bool configured_ = false;

        std::atomic<bool> running_{false};
        std::thread event_thread_;
        std::thread data_thread_;

        mutable std::mutex mutex_;
        std::condition_variable frame_cv_;
        std::condition_variable data_cv_;
        uint32_t pending_events_ = 0;
        std::vector<uint8_t> dma_buffer_;
        std::vector<uint8_t> latest_frame_;
        std::vector<uint8_t> delivery_frame_;
        uint64_t latest_sequence_ = 0;
        uint64_t delivered_sequence_ = 0;
        uint64_t wait_timeout_count_ = 0;
        bool stream_error_ = false;
        gv_stream_stats_t stats_{};
        mutable std::string last_error_;
    };
}
