#include "directshow_source.h"

DirectShowSource::~DirectShowSource()
{
    close();
}

DirectShowStartResult DirectShowSource::start(const DirectShowStartConfig &config)
{
    close();
    gcap_set_backend(GCAP_BACKEND_DSHOW);
    gcap_set_d3d_adapter(-1);

    DirectShowStartResult result;
    result.status = gcap_create(&handle_);
    if (result.status != GCAP_OK || !handle_)
    {
        result.failedApi = "gcap_create";
        if (handle_)
            close();
        return result;
    }

    const auto apply = [this, &result](gcap_status_t status, const char *api)
    {
        if (status == GCAP_OK)
            return true;
        result.status = status;
        result.failedApi = api;
        close();
        return false;
    };

    if (!apply(gcap_set_buffers(handle_, 6, 0), "gcap_set_buffers"))
        return result;
    if (!apply(gcap_set_callbacks(handle_, config.videoCallback, config.eventCallback, config.callbackUser),
               "gcap_set_callbacks"))
        return result;
    if (config.packetCallback &&
        !apply(gcap_set_frame_packet_callback(handle_, config.packetCallback, config.callbackUser),
               "gcap_set_frame_packet_callback"))
        return result;
    if (!apply(gcap_set_preview(handle_, &config.preview), "gcap_set_preview"))
        return result;
    if (!apply(gcap_set_profile(handle_, &config.profile), "gcap_set_profile"))
        return result;
    if (!apply(gcap_open2(handle_, config.deviceIndex), "gcap_open2"))
        return result;
    if (!apply(gcap_start(handle_), "gcap_start"))
        return result;

    running_ = true;
    return result;
}

void DirectShowSource::stop()
{
    if (handle_ && running_)
        gcap_stop(handle_);
    running_ = false;
}

void DirectShowSource::close()
{
    if (!handle_)
        return;
    stop();
    gcap_set_frame_packet_callback(handle_, nullptr, nullptr);
    gcap_set_callbacks(handle_, nullptr, nullptr, nullptr);
    gcap_close(handle_);
    handle_ = nullptr;
}

gcap_status_t DirectShowSource::signalStatus(gcap_signal_status_t *status) const
{
    return handle_ ? gcap_get_signal_status(handle_, status) : GCAP_ESTATE;
}

gcap_status_t DirectShowSource::runtimeInfo(gcap_runtime_info_t *info) const
{
    return handle_ ? gcap_get_runtime_info(handle_, info) : GCAP_ESTATE;
}

gcap_status_t DirectShowSource::deviceProperties(gcap_device_props_t *properties) const
{
    return handle_ ? gcap_get_device_props(handle_, properties) : GCAP_ESTATE;
}

gcap_status_t DirectShowSource::setPreview(const gcap_preview_desc_t &preview)
{
    return handle_ ? gcap_set_preview(handle_, &preview) : GCAP_ESTATE;
}

gcap_status_t DirectShowSource::setProcAmp(const gcap_procamp_t &values)
{
    return handle_ ? gcap_set_procamp(handle_, &values) : GCAP_ESTATE;
}

gcap_status_t DirectShowSource::procAmp(gcap_procamp_t *values) const
{
    return handle_ ? gcap_get_procamp(handle_, values) : GCAP_ESTATE;
}

gcap_status_t DirectShowSource::procAmpCapabilities(gcap_procamp_caps_t *capabilities) const
{
    return handle_ ? gcap_get_procamp_caps(handle_, capabilities) : GCAP_ESTATE;
}

gcap_status_t DirectShowSource::exportSnapshot(const gcap_snapshot_export_desc_t &description,
                                               gcap_snapshot_export_result_t *result) const
{
    return handle_ ? gcap_export_snapshot(handle_, &description, result) : GCAP_ESTATE;
}

gcap_status_t DirectShowSource::startRecording(const char *pathUtf8)
{
    return handle_ ? gcap_start_recording(handle_, pathUtf8) : GCAP_ESTATE;
}

void DirectShowSource::stopRecording()
{
    if (handle_)
        gcap_stop_recording(handle_);
}

gcap_status_t DirectShowSource::recordingInfo(gcap_recording_info_t *info) const
{
    return handle_ ? gcap_get_recording_info(handle_, info) : GCAP_ESTATE;
}

gcap_status_t DirectShowSource::recordingStats(gcap_recording_stats_t *stats) const
{
    return handle_ ? gcap_get_recording_stats(handle_, stats) : GCAP_ESTATE;
}
