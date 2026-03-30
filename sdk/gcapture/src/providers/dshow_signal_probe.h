#pragma once

#include <windows.h>
#include <dshow.h>
#include "gcapture.h"

struct DShowSignalProbeResult
{
    bool ok = false;
    int width = 0;
    int height = 0;
    int fps_num = 0;
    int fps_den = 1;
    GUID subtype = GUID_NULL;

    // Diagnostics: this probe currently reads DShow capture-pin media type,
    // not guaranteed true front-end HDMI/SDI signal metadata.
    bool from_get_format = false;
    bool from_stream_caps = false;
    bool filter_has_property_pages = false;
    bool capture_pin_has_property_pages = false;
    bool filter_has_ks_property_set = false;
    bool capture_pin_has_ks_property_set = false;
    bool filter_has_ks_control = false;
    bool capture_pin_has_ks_control = false;
    wchar_t friendly_name[128] = {};
    wchar_t device_path[512] = {};
};

bool dshow_probe_current_signal_by_index(int devIndex, DShowSignalProbeResult &out);
void dshow_dump_signal_diagnostics_by_index(int devIndex);
gcap_pixfmt_t gcap_subtype_to_pixfmt(const GUID &sub);
int gcap_pixfmt_bitdepth(gcap_pixfmt_t f);
const char *gcap_pixfmt_name(gcap_pixfmt_t f);
const char *gcap_subtype_name(const GUID &sub);
