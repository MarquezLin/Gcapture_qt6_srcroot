# Vendor Direct SDK Plan

## Current Direction

The vendor-direct path is now the XDMA backend in `sdk/gvendor`, exposed through
the Qt viewer as `GVendor Direct`.

The previous KS-direct diagnostic path has been removed. There is no separate
`gvendor_probe.exe` console tool anymore; debugging should use the viewer plus
the XDMA debug logging define.

## Build Options

```text
BUILD_QT_VIEWER_GVENDOR_BACKEND=ON
GVENDOR_XDMA_DEBUG_LOG=ON
```

`BUILD_QT_VIEWER_GVENDOR_BACKEND` includes `sdk/gvendor` and copies
`gvendor.dll` next to `qt6_viewer.exe`.

`GVENDOR_XDMA_DEBUG_LOG` enables verbose XDMA flow logs through
`OutputDebugStringA` and `stderr`.

## Active Components

- `sdk/gcapture`: product capture SDK and DirectShow / WinMF paths.
- `sdk/gvendor`: XDMA direct-capture backend.
- `apps/qt6_viewer`: viewer UI, including the optional `GVendor Direct` backend.

## Removed Components

- `apps/gvendor_probe`
- KS capture session implementation
- KS device enumerator implementation
