#pragma once

#include "xdma_backend_types.h"

#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace gvfg::internal
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

        xdma_status_t open_default();
        xdma_status_t open_device_index(size_t deviceIndex);
        xdma_status_t close();

        xdma_status_t set_input(gdriver_input_t input);
        xdma_status_t get_device_info(xdma_device_info_t &out) const;
        xdma_status_t get_signal_status(xdma_signal_status_t &out) const;
        xdma_status_t get_stream_stats(xdma_stream_stats_t &out) const;

        xdma_status_t set_event_callback(xdma_event_callback_t callback, void *user, uint32_t eventMask);
        xdma_status_t configure_stream(const xdma_stream_desc_t &desc);
        xdma_status_t start_stream();
        xdma_status_t stop_stream();
        xdma_status_t wait_frame(uint32_t timeoutMs, xdma_frame_t &out);
        xdma_status_t release_frame(const xdma_frame_t &frame);

        const char *last_error() const;

    private:
        xdma_status_t open_device(const XdmaDevice &device);
        void close_handles();

        HANDLE open_subdevice(const wchar_t *name) const;
        int read_device(HANDLE device, long address, DWORD size, uint8_t *buffer) const;
        int write_device(HANDLE device, long address, DWORD size, const uint8_t *buffer) const;
        bool read_user_reg(long address, uint32_t &out) const;
        bool write_user_reg(long address, uint32_t value) const;
        bool enable_user_event(uint32_t mask);
        bool disable_user_event(uint32_t mask);

        void event_thread_proc(uint32_t role, uint32_t irqBit);
        void data_thread_proc();
        bool start_data_worker();
        void stop_data_worker();
        void publish_frame(const uint8_t *data, size_t bytes);
        void pause_capture_for_plug_out();
        void resume_capture_after_plug_in();
        void handle_plug_in_frame_fix(const uint8_t *data, size_t bytes);
        void pulse_plug_in_frame_fix();
        void emit_event(xdma_event_type_t type, uint32_t irqBit, uint32_t irqMask) const;

        xdma_status_t fail(xdma_status_t status, const char *where, DWORD winerr = GetLastError()) const;
        void set_last_error(const std::string &message) const;
        void clear_last_error() const;

        uint32_t active_input_path() const;
        uint32_t event_mask(uint32_t role) const;
        uint32_t video_event_mask() const;
        uint32_t plug_in_event_mask() const;
        uint32_t plug_out_event_mask() const;
        uint32_t active_event_mask() const;
        long capture_enable_reg() const;
        size_t frame_size_bytes() const;

        std::wstring base_path_;
        std::wstring friendly_name_;

        HANDLE user_device_ = INVALID_HANDLE_VALUE;
        HANDLE c2h_device_[2] = {INVALID_HANDLE_VALUE, INVALID_HANDLE_VALUE};
        HANDLE event_device_[4] = {INVALID_HANDLE_VALUE, INVALID_HANDLE_VALUE, INVALID_HANDLE_VALUE, INVALID_HANDLE_VALUE};

        xdma_stream_desc_t stream_desc_{};
        uint32_t stream_bit_depth_ = 8;
        gdriver_input_t input_ = GDRIVER_INPUT_SDI;
        bool opened_ = false;
        bool configured_ = false;

        std::atomic<bool> running_{false};
        std::atomic<bool> capture_active_{false};
        std::atomic<bool> data_worker_stop_{false};
        std::atomic<int> save_frames_after_plug_in_{0};
        std::atomic<bool> fix_pulsed_after_plug_in_{false};
        std::thread event_thread_[4];
        std::thread data_thread_;

        mutable std::mutex mutex_;
        std::mutex worker_mutex_;
        mutable std::mutex event_callback_mutex_;
        std::condition_variable frame_cv_;
        std::condition_variable data_cv_;
        xdma_event_callback_t event_callback_ = nullptr;
        void *event_callback_user_ = nullptr;
        uint32_t event_mask_filter_ = XDMA_EVENT_MASK_DEFAULT;
        uint32_t pending_events_ = 0;
        std::vector<uint8_t> dma_buffer_;
        std::vector<uint8_t> latest_frame_;
        std::vector<uint8_t> delivery_frame_;
        uint64_t latest_sequence_ = 0;
        uint64_t delivered_sequence_ = 0;
        uint64_t wait_timeout_count_ = 0;
        bool stream_error_ = false;
        xdma_stream_stats_t stats_{};
        mutable std::string last_error_;
    };
}
