# win-capture-sdk_qt6 (split projects)

這個版本把原本「黏在一起」的 CMake 專案拆成 **SDK(DLL)** 與 **Demo App(EXE)** 兩個獨立子專案，避免 target / MOC / 連結設定互相干擾。

## 專案結構

- `sdk/gcapture`：核心擷取 SDK，產出 `gcapture.dll`（以及 MSVC 需要的 `gcapture.lib`）
- `sdk/gdisplay`：顯示卡/EDID 輔助 DLL，產出 `gdisplay.dll`
- `apps/qt6_viewer`：Qt6 Viewer demo，連結 `gcapture`/`gdisplay`，並在 build 後自動複製 DLL 到 exe 旁邊

輸出目錄：
- `build/.../bin`：`*.exe`、`*.dll`
- `build/.../lib`：`*.lib`

## Build（Qt6 + MSVC2022 x64）

在 Qt Creator：
1. 以 root `CMakeLists.txt` 開專案
2. 選 Desktop Qt 6.x MSVC2022 64bit kit
3. Configure
4. Build target `qt6_viewer`（會自動先 build `gcapture`、`gdisplay`）

### CaptureSDK.dll（可選）

`apps/qt6_viewer` 內含 `capturesdk/capture_sdk_source.*`，以 `LoadLibrary` 方式動態載入
`apps/qt6_viewer/third_party/capturesdk/bin/CaptureSDK.dll`，不需要 `.lib`。

