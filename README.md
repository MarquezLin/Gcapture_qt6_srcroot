# win-capture-sdk_qt6

這個 repo 目前分成幾個主要部分：

- `sdk/gcapture`: 既有 capture SDK，提供 DirectShow / WinMF 路線。
- `sdk/gdisplay`: display / EDID 相關 SDK。
- `sdk/gvfg`: 對外的 GVFG capture SDK facade，內含低階 XDMA driver access backend，給 VFG100 / GVFG 客戶使用。
- `apps/qt6_viewer`: Qt viewer demo，可以透過 `GVFG Direct` 使用 GVFG backend。

## Build

建議使用 Qt Creator：

1. 開啟 repo root 的 `CMakeLists.txt`。
2. 選擇 `Desktop Qt 6.x MSVC2022 64bit` kit。
3. Configure。
4. Build `qt6_viewer` 或 `all`。

輸出位置：

- `build/.../bin`: `.exe` / `.dll`
- `build/.../lib`: `.lib`

## GVFG SDK

`sdk/gvfg` 是對外的 GVFG capture SDK。客戶只需要 include：

```c
#include <gvfg_capture.h>
```

並 link：

```text
gvfg.lib
```

執行時需要放在 exe 旁邊：

```text
gvfg.dll
```

XDMA driver access 是 `gvfg.dll` 內部實作細節，客戶不需要也不應直接使用。

## Build Options

```text
BUILD_GVFG_SDK=ON
```

如果要看底層 XDMA flow debug log：

```text
GVFG_XDMA_DEBUG_LOG=ON
```

## Notes

- GVFG 是對外 SDK/API 名稱。
- XDMA 是目前底層 driver/backend 技術名稱。
- DirectShow / WinMF API 仍留在 `gcapture`，不混進 GVFG API。
