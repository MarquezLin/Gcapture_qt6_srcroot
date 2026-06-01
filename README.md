# win-capture-sdk_qt6

這個 repo 目前整理成三個主要部分：

- `sdk/gcapture`：正式 capture SDK，輸出 `gcapture.dll` / `gcapture.lib`。目前主線 frame path 是 DirectShow，並保留 WinMF backend。
- `sdk/gdisplay`：顯示器 / EDID 相關 SDK，輸出 `gdisplay.dll` / `gdisplay.lib`。
- `apps/qt6_viewer`：Qt6 viewer demo，link `gcapture`、`gdisplay`，並可透過 `GVendor Direct` 使用 standalone XDMA SDK。

`sdk/gxdma` 是 standalone XDMA capture SDK，提供給 viewer 的 `GVendor Direct` 使用；`sdk/gvendor` 則是底層 XDMA driver access layer。

## Build

建議使用 Qt Creator：

1. 開啟 repo root 的 `CMakeLists.txt`。
2. 選擇 `Desktop Qt 6.x MSVC2022 64bit` kit。
3. Configure。
4. Build `qt6_viewer` 或 `all`。

輸出通常會在：

- `build/.../bin`：`.exe` / `.dll`
- `build/.../lib`：`.lib`

## 正式 SDK 方向

目前建議的正式產品架構是：

```text
App / qt6_viewer
  ↓
gcapture.dll
  ├─ frame path：DirectShow
  ├─ preview / snapshot / recording
  └─ future control path：private IOCTL / KS private property
        ↓
      GIGABYTE capture driver
```

也就是說，使用者面對的是 `gcap_*` API；底層是否走 DirectShow、private IOCTL、或未來 shared buffer，都是 SDK 內部實作細節。

## Private IOCTL 預留

`sdk/gdriver_shared/include` 放 driver 與 SDK 未來共用的 ABI / IOCTL contract：

- `gdriver_abi.h`
- `gdriver_control_codes.h`

`sdk/gcapture/src/control/gdriver_control_client.*` 是未來 private IOCTL control path 的 user-mode client 空殼。目前 driver 尚未 expose `GUID_DEVINTERFACE_GDRIVER_CAPTURE`，所以這條路徑尚未接到正式 API。

短期建議先讓 driver 提供控制/資訊 IOCTL：

- `IOCTL_GDRIVER_GET_DEVICE_INFO`
- `IOCTL_GDRIVER_GET_SIGNAL_STATUS`
- `IOCTL_GDRIVER_SET_INPUT`
- `IOCTL_GDRIVER_GET_STATS`

Minimal customer-facing Qt sample:
```text
samples/gxdma_qt_preview
```

Build target:
```text
gxdma_qt_preview
```

frame path 可以先維持 DirectShow，等 private shared-buffer frame API 成熟後再替換。

## Standalone XDMA SDK

`sdk/gxdma` is the standalone XDMA capture SDK used by the Qt viewer `GVendor Direct` option. It uses `sdk/gvendor` for low-level XDMA driver access, but it is intentionally separate from `sdk/gcapture` while the XDMA driver path is still under development. The old KS-direct path and `gvendor_probe.exe` console tool have been removed.

Customer-facing XDMA applications should include only `sdk/gxdma/include/gxdma_capture.h` and link `gxdma.lib`. `sdk/gvendor` and `sdk/gdriver_shared` are internal implementation details for the XDMA backend and are not part of the customer API surface.

Enable the standalone XDMA SDK/viewer integration with:
```text
BUILD_GXDMA_SDK=ON
```

Enable verbose XDMA flow logging with:
```text
GVENDOR_XDMA_DEBUG_LOG=ON
```

## 舊 CaptureSDK 狀態

舊的外部 `CaptureSDK.dll` / `CaptureSDK.lib` 整合已移除。正式路線先收斂到 `gcapture.dll`，未來 vendor/private control 也應接在 `gcapture` 之下。

## 打包

`pack_qt6_viewer.bat` 會複製 viewer 需要的 DLL。預設產品 build 需要：

- `gcapture.dll`
- `gdisplay.dll`

只有在開啟 `BUILD_GXDMA_SDK=ON` 時，viewer 才需要 `gxdma.dll` / `gvendor.dll`。
