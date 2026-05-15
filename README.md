# win-capture-sdk_qt6

這個 repo 目前整理成三個主要部分：

- `sdk/gcapture`：正式 capture SDK，輸出 `gcapture.dll` / `gcapture.lib`。目前主線 frame path 是 DirectShow，並保留 WinMF backend。
- `sdk/gdisplay`：顯示器 / EDID 相關 SDK，輸出 `gdisplay.dll` / `gdisplay.lib`。
- `apps/qt6_viewer`：Qt6 viewer demo，預設只 link 正式產品 SDK：`gcapture` 與 `gdisplay`。

`sdk/gvendor` 目前定位為工程診斷用 KS backend，不是預設產品路線。

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

frame path 可以先維持 DirectShow，等 private shared-buffer frame API 成熟後再替換。

## GVendor 診斷工具

`sdk/gvendor` 目前保留兩種診斷 backend：

- KS-direct：直接透過 `KsCreatePin` / `IOCTL_KS_READ_STREAM` 從 AVStream capture pin 讀 frame。

目前提供的 driver package 沒有 expose XDMA user-mode interface，所以專案先不提供 XDMA backend 開關。KS-direct 已驗證可以抓到 frame，但目前定位是診斷 / fallback，不是 viewer 預設 backend。

需要診斷工具時，在 CMake 開啟：

```text
BUILD_GVENDOR_PROBE=ON
```

常用參數：

```text
gvendor_probe.exe list
gvendor_probe.exe sdi
gvendor_probe.exe sdi --frames 100
gvendor_probe.exe sdi --device 2
```

如果真的要把 `GVendor Direct` 放回 viewer 下拉選單，需另外開啟：

```text
BUILD_QT_VIEWER_GVENDOR_BACKEND=ON
```

## 舊 CaptureSDK 狀態

舊的外部 `CaptureSDK.dll` / `CaptureSDK.lib` 整合已移除。正式路線先收斂到 `gcapture.dll`，未來 vendor/private control 也應接在 `gcapture` 之下。

## 打包

`pack_qt6_viewer.bat` 會複製 viewer 需要的 DLL。預設產品 build 需要：

- `gcapture.dll`
- `gdisplay.dll`

只有在開啟 `BUILD_QT_VIEWER_GVENDOR_BACKEND=ON` 時，viewer 才需要 `gvendor.dll`。
