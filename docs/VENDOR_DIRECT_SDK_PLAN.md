# Vendor Direct SDK Plan

## Current Direction

The vendor-direct path is now the standalone XDMA SDK in `sdk/gxdma`, backed by
the low-level driver access in `sdk/gvendor`, and exposed through the Qt viewer
as `GVendor Direct`.

The previous KS-direct diagnostic path has been removed. There is no separate
`gvendor_probe.exe` console tool anymore; debugging should use the viewer plus
the XDMA debug logging define.

## Build Options

```text
BUILD_GXDMA_SDK=ON
GVENDOR_XDMA_DEBUG_LOG=ON
```

`BUILD_GXDMA_SDK` includes `sdk/gvendor`, builds `gxdma.dll`, and copies
`gxdma.dll` / `gvendor.dll` next to `qt6_viewer.exe`.

`GVENDOR_XDMA_DEBUG_LOG` enables verbose XDMA flow logs through
`OutputDebugStringA`.

## Active Components

- `sdk/gcapture`: product capture SDK and DirectShow / WinMF backends.
- `sdk/gxdma`: standalone XDMA capture SDK facade.
- `sdk/gvendor`: low-level XDMA driver access.
- `apps/qt6_viewer`: viewer UI; `GVendor Direct` uses `gxdma` directly.

## Customer API

See `docs/GXDMA_CUSTOMER_API.md` for the public XDMA API usage guide.

## Removed Components

- `apps/gvendor_probe`
- KS capture session implementation
- KS device enumerator implementation
