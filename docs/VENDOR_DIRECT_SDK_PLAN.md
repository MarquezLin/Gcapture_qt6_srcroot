# Vendor SDK 整理方向

## 結論

目前正式產品 SDK 不需要把 KS-direct 當主線。

建議收斂成：

```text
App / customer code
  ↓
gcapture.dll
  ├─ frame path：DirectShow
  ├─ preview / snapshot / recording
  └─ future vendor control：private IOCTL / KS private property
        ↓
      GIGABYTE capture driver
```

`gvendor` 保留為工程診斷工具，用來驗證 KS pin、driver format、以及未來 private frame path 的行為差異。

## 為什麼不把 KS 當正式主線

如果正式產品策略是：

```text
畫面走 DirectShow
自家控制與資訊走 private IOCTL
```

那 KS-direct 不是必要條件。DirectShow 已經能提供穩定 frame path，而且 SDK 對外仍是自家的 `gcap_*` API。KS-direct 的價值主要是：

- 診斷 driver 的 KS/AVStream pin。
- 在不經過 DirectShow graph 的情況下驗證 frame delivery。
- 作為未來 private frame API 的對照測試。

## 目前 driver 狀態

檢查 `D:/GIGADirver/output.zip` 與 `D:/GIGADirver/avshws 2.zip` 後，現有 driver 架構是：

```text
FPGA / XDMA
  ↓
driver internal DMA ring buffer
  ↓
AVStream / KS capture pin
  ↓
DirectShow / KS clients
```

driver source 有 `xdma_public.h`，其中定義：

```text
GUID_DEVINTERFACE_XDMA = {74c7e4a9-6d5d-4a70-bc0d-20691dff9e9d}
\user
\c2h_0
\h2c_0
\event_0 ...
```

但目前 driver code / INF 沒有註冊 user-mode XDMA interface：

- 沒有 `WdfDeviceCreateDeviceInterface`
- 沒有 `IoRegisterDeviceInterface`
- 沒有 `IoCreateSymbolicLink`
- INF 只註冊 `KSCATEGORY_CAPTURE` / `KSCATEGORY_VIDEO` / `KSCATEGORY_AUDIO`

因此目前不把 XDMA direct path 放進可選 backend，避免工程診斷工具出現「可選但必定找不到 device」的狀態。

## CMake 開關

預設產品 build 不會把 gvendor 接進 viewer。

```text
BUILD_GVENDOR_PROBE=OFF
BUILD_QT_VIEWER_GVENDOR_BACKEND=OFF
```

需要診斷 KS 時才開：

```text
BUILD_GVENDOR_PROBE=ON
```

如果要把實驗性的 `GVendor Direct` 放回 viewer 下拉選單：

```text
BUILD_QT_VIEWER_GVENDOR_BACKEND=ON
```

## 程式碼定位

- `sdk/gcapture`
  - 正式 SDK。
  - DirectShow / WinMF frame backend。
  - 未來 private IOCTL control path 也應放在這裡。

- `sdk/gcapture/src/control/gdriver_control_client.*`
  - 未來 private IOCTL client 的預留位置。
  - 目前不接正式 API，因為 driver 尚未 expose `GUID_DEVINTERFACE_GDRIVER_CAPTURE`。

- `sdk/gdriver_shared/include`
  - driver 與 SDK 共用 ABI / IOCTL contract。
  - 目前是 future contract，不代表現有 driver 已支援。

- `sdk/gvendor`
  - 診斷 SDK。
  - KS-direct 已驗證可抓 frame。
  - XDMA-direct 暫不放入 build；要等 driver 明確 expose user-mode interface 再重啟。

- `apps/gvendor_probe`
  - 工程診斷工具。
  - 不在預設 `all` target 中建立。

## 建議下一步

1. 保持 viewer 預設走 `gcapture` / DirectShow。
2. 與 driver 同事確認 private IOCTL 第一版 contract。
3. 第一版 private IOCTL 先做 control/status，不碰 frame DMA：
   - `GET_DEVICE_INFO`
   - `GET_SIGNAL_STATUS`
   - `SET_INPUT`
   - `GET_STATS`
4. 等 control path 穩定後，再討論 private shared-buffer frame path。
5. `gvendor_probe` 保留作為 KS 對照驗證工具。
