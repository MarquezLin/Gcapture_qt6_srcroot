#pragma once

// Share the same import/export macro as the main SDK API.
#include "capture_sdk.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CAP_LOG_INFO = 0,
    CAP_LOG_WARN = 1,
    CAP_LOG_ERR  = 2
} cap_log_level_t;

typedef void (*cap_log_callback_t)(cap_log_level_t level, const char* msg);

// Register a callback to receive CaptureSDK logs (thread context: CaptureSDK internal threads / pump thread).
// Passing NULL disables callback.
CAPSDK_API void cap_set_log_callback(cap_log_callback_t cb);

#ifdef __cplusplus
}
#endif
