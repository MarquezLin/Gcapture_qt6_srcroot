# SDK Refactor Phase 3-2: SDK Log Callback

This phase adds a public SDK logging callback so applications can receive
WinMF/DShow/snapshot/recording diagnostic messages without depending on the Qt
viewer's debug window.

## Public API

Added to `sdk/gcapture/include/gcapture.h`:

```c
typedef enum
{
    GCAP_LOG_TRACE = 0,
    GCAP_LOG_DEBUG = 1,
    GCAP_LOG_INFO  = 2,
    GCAP_LOG_WARN  = 3,
    GCAP_LOG_ERROR = 4
} gcap_log_level_t;

typedef void (*gcap_log_callback_t)(
    gcap_log_level_t level,
    const char *message_utf8,
    void *user);

GCAP_API void gcap_set_log_callback(gcap_log_callback_t cb, void *user);
```

## Behavior

- The callback is process-wide.
- Passing `NULL` disables the callback.
- The callback may run on SDK/backend worker threads; UI applications must
  marshal messages back to the UI thread.
- Messages are UTF-8 and only valid during the callback call.
- On Windows the SDK still writes to `OutputDebugStringA` as a fallback.

## Internal changes

Added:

- `sdk/gcapture/src/core/logging.h`
- `sdk/gcapture/src/core/logging.cpp`

Most internal `OutputDebugStringA/W` calls in `sdk/gcapture` now route through
this logging helper, so clients can receive backend diagnostics.

## Qt viewer change

`apps/qt6_viewer/mainwindow.cpp` installs the SDK log callback and forwards
messages to the existing debug log dock and Qt log file.
