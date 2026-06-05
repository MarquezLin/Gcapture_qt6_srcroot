# GVFG Direct SDK Plan

## Current Direction

GVFG Direct 目前改成使用獨立的 GVFG SDK：

- 對外 facade：`sdk/gvfg`
- 低階 driver access：`sdk/gvfg/src/backend/xdma`
- Viewer 整合：`apps/qt6_viewer` 的 `GVFG Direct`

GVFG 是客戶看到的 SDK/API 名稱。XDMA 只保留為底層 driver/backend 說明。

## Build Options

```text
BUILD_GVFG_SDK=ON
BUILD_GVFG_SAMPLES=ON
GVFG_XDMA_DEBUG_LOG=ON
```

`BUILD_GVFG_SDK` 會 build `gvfg.dll`，並把 `gvfg.dll` 複製到 viewer 輸出資料夾。XDMA backend 會編進 `gvfg.dll`，不再另外產生 backend DLL。

`GVFG_XDMA_DEBUG_LOG` 會開啟底層 XDMA flow log。

## Active Components

- `sdk/gvfg`: customer-facing GVFG capture SDK。
- `sdk/gvfg/src/backend/xdma`: internal low-level XDMA driver access。
- `apps/qt6_viewer`: viewer demo，`GVFG Direct` 透過 GVFG SDK 啟動。
- `samples/gvfg_qt_preview`: 最小客戶 sample。

## Customer API

請看：

```text
docs/GVFG_CUSTOMER_API.md
```

## Removed Components

- old standalone XDMA probe app
- KS capture session implementation
- KS device enumerator implementation
