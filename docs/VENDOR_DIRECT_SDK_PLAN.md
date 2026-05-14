# Vendor-Direct SDK 計畫

## 目標

建立一套自己的 vendor SDK，讓應用程式可以直接搭配目前的 GIGABYTE capture driver 工作，而不是依賴 Media Foundation 或 DirectShow 作為主要擷取路徑。

目前第一版先走 Windows KS / AVStream direct path：

```text
Qt viewer / sample apps
        |
     GVendor KS wrapper
        |
     gvendor.dll
        |
  KS / AVStream device interface
        |
 GIGABYTE capture driver
```

後續若 driver 同事提供 private IOCTL，可以在 `gvendor` 內新增 private transport，對上層 API 維持穩定。

## Repo 結構

- `sdk/gdriver_shared/include/gdriver_abi.h`
  - SDK 與未來 driver private IOCTL 共用的 ABI struct / enum。
  - 不 include Windows headers，避免污染 user-mode public header。

- `sdk/gdriver_shared/include/gdriver_control_codes.h`
  - Windows-only private IOCTL / GUID 定義。
  - 未來如果 driver 加 private IOCTL，會從這裡共用控制碼。

- `sdk/gvendor/include/gvendor.h`
  - vendor SDK 的 public C API。
  - 包含 enumerate、open、configure、start、wait frame、stop 等最小 API。

- `sdk/gvendor/src/*`
  - 目前實作 KS direct device enumeration、pin creation、stream read。
  - 已排除舊的 `GIGABYTE_Vision Device` 虛擬測試 driver。
  - 預設選擇真實 driver 的 YUY2 video capture filter endpoint。

- `apps/gvendor_probe`
  - `gvendor` 診斷工具。
  - 預設不 build，需要時用 `BUILD_GVENDOR_PROBE=ON` 開啟。

- `apps/qt6_viewer/gvendor`
  - Qt viewer 的 `GVendor KS` backend wrapper。
  - 取代舊的外部 CaptureSDK 整合。

## 目前已驗證

使用真實 `GIGABYTE Capture Card`：

- `gvendor_probe.exe list` 可列出真實 capture card KS endpoint。
- 預設開啟 driver 的 YUY2 video endpoint。
- `sdi` 可成功：
  - configure stream
  - start stream
  - wait frame
  - 連續抓 100 張 frame

已驗證格式：

```text
SDI / YUY2 / 1920x1080
frame size = 4147200 bytes
stride = 3840
```

## Qt Viewer 整合

`qt6_viewer` 的 backend combo 目前有 `GVendor KS`。

`GVendor KS` 會：

1. 呼叫 `gv_open_default()`。
2. 設定 SDI input。
3. configure `YUY2 1920x1080`。
4. start stream。
5. 背景 thread 持續 `gv_wait_frame()`。
6. 將 YUY2 frame 轉成 `QImage::Format_RGB888`。
7. 送到 preview window 顯示。

## 舊 CaptureSDK 狀態

舊的外部 CaptureSDK 整合已移除：

- 不再 link `CaptureSDK.lib`。
- 不再 copy `CaptureSDK.dll`。
- 移除 `apps/qt6_viewer/third_party/capturesdk`。
- viewer wrapper 已改名為 `GVendorSource`。

## 下一步

建議後續工作：

1. 支援 HDMI / SDI 真正 input routing。
2. 從 driver 取得真實 signal lock、解析度、fps。
3. 支援更多 pixel format，例如 RGB24、Y210。
4. 將 YUY2 轉 RGB 移到更有效率的路徑，例如 shader 或 SIMD。
5. 若 driver 提供 private IOCTL，新增 private transport，但保留 `gvendor.h` API 不變。
6. 將 recording path 也接上 `GVendor KS`，目前 viewer 的 recording 仍只支援 `gcapture` backend。
