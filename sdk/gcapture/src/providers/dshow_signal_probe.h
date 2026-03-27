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
};

bool dshow_probe_current_signal_by_index(int devIndex, DShowSignalProbeResult &out);
gcap_pixfmt_t gcap_subtype_to_pixfmt(const GUID &sub);
int gcap_pixfmt_bitdepth(gcap_pixfmt_t f);
const char *gcap_pixfmt_name(gcap_pixfmt_t f);
const char *gcap_subtype_name(const GUID &sub);
