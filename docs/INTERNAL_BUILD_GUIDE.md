# qt6/srcroot 內部建置與特殊功能說明

本文件供內部開發使用，說明 `qt6/srcroot` 如何連接外部 `GVFG_Standalone`、如何透過 CMake 開啟 GVFG backend 與內部工具，以及 runtime helper 的部署方式。

## 1. 專案定位

`srcroot` 是完整的 Qt viewer 應用程式專案，主要包含：

- `sdk/gcapture`：Windows Media Foundation／DirectShow capture SDK。
- `sdk/gdisplay`：display 與 EDID 功能。
- `apps/qt6_viewer`：正式 viewer GUI。
- 外部 `GVFG_Standalone`：GVFG direct capture、preview 與 GPU conversion。

`CaptureDemo` 不屬於這個正式整合流程。GVFG 的 source 應修改於 `GVFG_Standalone`；viewer 上層整合修改於 `apps/qt6_viewer`。

## 2. CMake 選項

| 變數 | 類型／預設 | 功能 |
|---|---|---|
| `GVFG_STANDALONE_ROOT` | PATH | Standalone source root，用來取得公開 header。 |
| `GVFG_STANDALONE_BUILD_DIR` | PATH | Standalone build root，或直接包含 `bin`、`lib` 的 build 目錄。 |
| `FFMPEG_ROOT` | PATH | FFmpeg 的 `include/lib/bin` 根目錄。 |
| `EDID_DECODE_EXE` | FILEPATH／專案內預設 | 指向 `edid-decode.exe`；建置後自動複製到 viewer 執行目錄。 |

GVFG backend、GVFG 錄影與 FFmpeg LGPL 檢查都是必要功能，不提供關閉選項。專案版本只修改根目錄 `project(win_capture_sdk VERSION ...)`；gcapture 與 viewer 共用此版本。

## 3. GVFG 特殊功能如何開啟

Windows build 會固定加入兩個 compile definitions：

```text
QT6_VIEWER_ENABLE_GVFG_BACKEND
QT6_VIEWER_ENABLE_GVFG_INTERNAL_TOOLS
```

它們不是目前可分開切換的 CMake cache options。

### `QT6_VIEWER_ENABLE_GVFG_BACKEND`

會啟用：

- Backend 選單中的 `GVFG Direct`。
- GVFG 裝置與 channel 列舉。
- YUY2／Y210 capture 與 preview。
- GVFG runtime FPS、signal status、frame loss 顯示。
- 啟動 log 顯示 `gvfg_get_version()` 回傳的實際 GVFG DLL 版本。
- GVFG snapshot：PNG/TIFF，以及原生 `_source_yuy2.raw` 或 `_source_y210.raw`。
- GPU conversion 與 FFmpeg 錄影整合。

### `QT6_VIEWER_ENABLE_GVFG_INTERNAL_TOOLS`

會把 register dialog 編入程式，但預設仍隱藏。必須加啟動參數才會顯示：

```bat
qt6_viewer.exe --gvfg-registers
```

這會顯示 GVFG register 工具，供內部 read/write register 與硬體診斷使用。不要在客戶版本公開這個入口。

RAW Pixel Inspector 也屬於內部分析用途，可檢查 YUY2（`Y0 U0 Y1 V0`）、Y210、BGRA8、RGBA8 與 ABGR2101010 raw frame。

## 4. 建置順序

### 第一步：先建置 Standalone

```bat
cd C:\Users\mark\Desktop\GcaptureSDK\GVFG_Standalone

cmake -S . -B build_release ^
  -DBUILD_GVFG_SAMPLES=ON ^
  -DCMAKE_PREFIX_PATH=C:\Qt\6.10.2\msvc2022_64

cmake --build build_release --config Release --target gvfg_qt_preview
```

### 第二步：configure srcroot

```bat
cd C:\Users\mark\Desktop\GcaptureSDK\qt6\srcroot

cmake -S . -B build_release -G Ninja ^
  -DCMAKE_BUILD_TYPE=Release ^
  -DCMAKE_PREFIX_PATH=C:\Qt\6.10.2\msvc2022_64 ^
  -DGVFG_STANDALONE_ROOT=C:\Users\mark\Desktop\GcaptureSDK\GVFG_Standalone ^
  -DGVFG_STANDALONE_BUILD_DIR=C:\Users\mark\Desktop\GcaptureSDK\GVFG_Standalone\build_release
```

### 第三步：建置 viewer

```bat
cmake --build build_release --target qt6_viewer
```

執行檔位於：

```text
build_release/bin/qt6_viewer.exe
```

若使用 Qt Creator，將相同的 `-D...` 內容加入 Projects > Build > CMake configuration，修改 CMake 後要執行一次 Re-configure，不只是 Build。

## 5. Standalone build 自動搜尋

若 `GVFG_STANDALONE_BUILD_DIR` 指向 `GVFG_Standalone/build`，而不是直接指向某個 configuration，top-level CMake 會掃描其子目錄：

1. 優先尋找同 compiler 且名稱以 `-Release` 或目前 `CMAKE_BUILD_TYPE` 結尾的目錄。
2. 確認同時存在 `gvfg.dll/lib` 與 `gvfg_preview.dll/lib`。
3. 找不到偏好版本時，才 fallback 到第一個完整 build。

為避免誤連 Debug 或舊 DLL，內部正式測試仍建議直接指定完整 build directory。

## 6. 建置後自動部署

`qt6_viewer` 的 POST_BUILD 會把下列檔案複製到 `$<TARGET_FILE_DIR:qt6_viewer>`：

- `gcapture.dll`
- `gdisplay.dll`
- `gvfg.dll`
- `gvfg_preview.dll`
- `third_party/ffmpeg/bin` 內的 runtime DLL 與工具
- `EDID_DECODE_EXE` 指定的 `edid-decode.exe`

`EDID_DECODE_EXE` 必須是 FILEPATH。預設值為：

```text
third_party/edid-decode/vs/x64/Release/edid-decode.exe
```

若 CMakeCache 曾留下舊的 `EDID_DECODE_EXE:BOOL=OFF`，重新 configure 後會自動修正成預設 FILEPATH。

## 7. Release 與 Debug 的差異

`srcroot` 的 GVFG backend/internal tools 固定啟用，不是由 Debug 或 CMake 開關控制。

- Release：仍可使用 GVFG Direct、register tool（需參數）、RAW Inspector 與錄影。
- Debug：viewer 本身增加 debug symbols；如果它連到 Standalone Debug build，Standalone 端才會同時開啟 `GVFG_INTERNAL_DIAGNOSTICS`。

要調查 DMA、IRQ、held frame 或 preview stall，建議 viewer 與 Standalone 都使用 Debug。要量測 FPS 與正式效能，兩者都使用 Release。

## 8. 常見問題

### `GVFG_PIXFMT_*` 未宣告

viewer source 與 Standalone header 版本不同。同步 enum 名稱後，重新 configure，確認 compiler include path 指向正確 `GVFG_STANDALONE_ROOT`。

### 找不到 `gvfg.dll`／`gvfg_preview.dll`

確認 `GVFG_STANDALONE_BUILD_DIR` 下有：

```text
bin/gvfg.dll
bin/gvfg_preview.dll
lib/gvfg.lib
lib/gvfg_preview.lib
```

### EDID 顯示 `edid-decode.exe not found`

先確認 viewer 的 `bin` 目錄有 `edid-decode.exe`。若沒有：

1. 確認 `EDID_DECODE_EXE` 指向存在的檔案。
2. Re-configure CMake。
3. 關閉正在執行的 viewer。
4. 重新 build `qt6_viewer`，讓 POST_BUILD 執行。

### Linker `LNK1104: qt6_viewer.exe`

viewer 正在執行，Windows 鎖住 EXE。關閉程式後重建。

### 找不到 `type_traits`／`utility`

沒有載入 Visual Studio developer environment。從 Qt Creator 的 MSVC kit 建置，或先執行：

```bat
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat" -arch=x64
```

### 修改 CMake 後功能沒有出現

Build 不一定會更新 cache。執行 Re-configure，並檢查 configure output 是否包含：

```text
Using external GVFG SDK: ...
qt6_viewer: GVFG FFmpeg recording enabled: ...
```
