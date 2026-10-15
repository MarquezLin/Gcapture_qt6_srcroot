#pragma once

#include <gcapture.h>
#include "backend/capture_source.h"

struct DirectShowStartConfig
{
    int deviceIndex = -1;
    gcap_preview_desc_t preview{};
    gcap_profile_t profile{};
    gcap_on_video_cb videoCallback = nullptr;
    gcap_on_error_cb eventCallback = nullptr;
    gcap_on_frame_packet_cb packetCallback = nullptr;
    void *callbackUser = nullptr;
};

struct DirectShowStartResult
{
    gcap_status_t status = GCAP_OK;
    const char *failedApi = nullptr;
};

class DirectShowSource final : public CaptureSource
{
public:
    DirectShowSource() = default;
    ~DirectShowSource() override;

    DirectShowSource(const DirectShowSource &) = delete;
    DirectShowSource &operator=(const DirectShowSource &) = delete;

    DirectShowStartResult start(const DirectShowStartConfig &config);
    CaptureBackend backend() const override { return CaptureBackend::DirectShow; }
    void stop() override;
    void close() override;

    bool isOpen() const override { return handle_ != nullptr; }
    bool isRunning() const override { return running_; }

    gcap_status_t signalStatus(gcap_signal_status_t *status) const;
    gcap_status_t runtimeInfo(gcap_runtime_info_t *info) const;
    gcap_status_t deviceProperties(gcap_device_props_t *properties) const;
    gcap_status_t setPreview(const gcap_preview_desc_t &preview);
    gcap_status_t setProcAmp(const gcap_procamp_t &values);
    gcap_status_t procAmp(gcap_procamp_t *values) const;
    gcap_status_t procAmpCapabilities(gcap_procamp_caps_t *capabilities) const;
    gcap_status_t exportSnapshot(const gcap_snapshot_export_desc_t &description,
                                 gcap_snapshot_export_result_t *result) const;
    gcap_status_t startRecording(const char *pathUtf8);
    gcap_status_t setRecordingSegmentDuration(uint32_t seconds);
    void stopRecording();
    gcap_status_t recordingInfo(gcap_recording_info_t *info) const;
    gcap_status_t recordingStats(gcap_recording_stats_t *stats) const;

private:
    gcap_handle handle_ = nullptr;
    bool running_ = false;
};
