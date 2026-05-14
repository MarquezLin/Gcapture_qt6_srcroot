# win-capture-sdk_qt6

這個 repo 目前拆成多個 CMake target：SDK DLL、vendor-direct SDK、以及 Qt6 viewer demo。

## 專案結構

- `sdk/gcapture`：原本的 capture SDK，輸出 `gcapture.dll` / `gcapture.lib`，目前仍提供 DirectShow / WinMF 等既有 backend。
- `sdk/gdisplay`：顯示器 / EDID 相關 SDK，輸出 `gdisplay.dll` / `gdisplay.lib`。
- `sdk/gvendor`：新的 vendor-direct SDK，輸出 `gvendor.dll` / `gvendor.lib`，目前走 KS direct，不依賴 MF / DirectShow 抓 frame。
- `apps/qt6_viewer`：Qt6 viewer demo，會 link `gcapture`、`gdisplay`、`gvendor`。
- `apps/gvendor_probe`：`gvendor` 診斷工具，預設不 build，需要時用 `BUILD_GVENDOR_PROBE=ON` 開啟。

## Build

建議使用 Qt Creator：

1. 開啟 repo root 的 `CMakeLists.txt`。
2. 選擇 `Desktop Qt 6.x MSVC2022 64bit` kit。
3. Configure。
4. Build `qt6_viewer` 或 `all`。

輸出通常會在：

- `build/.../bin`：`.exe` / `.dll`
- `build/.../lib`：`.lib`

## GVendor KS Backend

`qt6_viewer` 內的 `GVendor KS` backend 使用：

- `apps/qt6_viewer/gvendor/gvendor_source.h`
- `apps/qt6_viewer/gvendor/gvendor_source.cpp`
- `sdk/gvendor/include/gvendor.h`

目前已取代舊的外部 CaptureSDK 整合；不再需要 `CaptureSDK.dll`、`CaptureSDK.lib` 或 `apps/qt6_viewer/third_party/capturesdk`。

目前驗證成功的路徑：

- 裝置：`GIGABYTE Capture Card`
- endpoint：driver 的 YUY2 video capture filter
- 格式：`SDI / YUY2 / 1920x1080`
- 路徑：KS direct `configure -> start -> wait_frame`

## gvendor_probe

`gvendor_probe` 是診斷工具，不會在預設 `all` target 中建立。需要時可在 CMake 設定：

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

## 打包

`pack_qt6_viewer.bat` 會複製 viewer 需要的 DLL，包括：

- `gcapture.dll`
- `gdisplay.dll`
- `gvendor.dll`

FFmpeg runtime DLL 仍從 `third_party/ffmpeg/bin` 複製。
