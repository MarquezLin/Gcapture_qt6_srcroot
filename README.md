# win-capture-sdk_qt6

This repository contains the Qt 6 viewer and the local capture/display SDK code
used by the viewer.

Backend ownership and maintenance status are documented in
[`docs/BACKEND_BOUNDARIES.md`](docs/BACKEND_BOUNDARIES.md).

The source tree is intentionally limited to these runtime paths:

- `sdk/gcapture`: local DirectShow capture SDK.
- `sdk/gdisplay`: local display / EDID helper SDK used by the viewer.
- `apps/qt6_viewer`: Qt viewer application.
- External `GVFG_Standalone`: GVFG capture SDK imported as prebuilt `gvfg*.dll`
  and `gvfg*.lib`.

Legacy low-level hardware backend source is not part of this project. GVFG
hardware access lives behind the external GVFG SDK.

## Build

Recommended flow:

1. Open this repository root in Qt Creator.
2. Select a Desktop Qt 6 MSVC 64-bit kit.
3. Configure.
4. Build `qt6_viewer` or `all`.

Build outputs are written under the CMake build directory:

- `bin`: executables and runtime DLLs.
- `lib`: import libraries.

### Windows Console log

在 Qt Creator 的專案 Build Settings／CMake 設定中新增或修改 BOOL 選項
`GVFG_VIEWER_ENABLE_CONSOLE`，重新執行 CMake 並編譯 `qt6_viewer`：

- `ON`：使用 Console subsystem。從檔案總管啟動時會顯示黑色 Console 視窗；
  從命令列啟動時使用該命令列的 Console，Qt GUI 仍正常顯示。
- `OFF`：使用 Windows GUI subsystem，不自動開啟 Console。

命令列設定相同選項時使用 `-DGVFG_VIEWER_ENABLE_CONSOLE=ON` 或 `OFF`。
這個開關對 Debug／Release 都有效，只影響 Viewer，不影響 SDK DLL。
原有 Qt message handler 已將 log 寫至 `stdout`／`stderr`，因此直接共用既有的
Qt／SDK log；GUI log 面板、`logs/` 下的 log 檔案維持不變。
Qt Creator 中的訊息仍由 Application Output 顯示；是否另開 Console 視窗取決於
IDE 的執行設定。關閉 Console 視窗可能結束整個程式，請用 GUI 正常離開。

### OSD frame 交付耗時診斷

Windows 下 `GVFG_VIEWER_ENABLE_CONSOLE=ON` 時，同時編入 GVFG frame 交付診斷；
設為 `OFF` 就不編入計時輸出路徑。分段計時使用新增的內部
`gvfg_debug_read_channel_frame_profiled()`；請先重新編譯 GVFG SDK 的 `gvfg.dll`／`gvfg.lib`，
再編譯 Viewer。客戶用的 `gvfg_capture.h` 與 frame ABI 不變。
Start 後從既有 Qt log、Console、GUI log 面板與 log 檔案查看：

```text
[OSD ON] 300 張耗時，單位 ms（平均 / 最大）
  1. 取得原始畫面（含必要的複製）：... / ...
  2. 準備文字與輸出空間：... / ...
  3. 上傳原始畫面的疊字區域：... / ...
  4. 提交 GPU 疊字工作：... / ...
  5. CPU 額外複製完整畫面：... / ...
  6. 等待 GPU 完成並取得結果：... / ...
  7. CPU 將疊字結果貼回完整畫面：... / ...
  8. 交付前釋放原始擷取畫面：... / ...
  取資料到交付上層總耗時（不含等下一張）：... / ...
```

每收滿 300 張有效 frame 就依處理順序顯示該組的平均耗時與最大耗時，單位毫秒，
然後從下一張開始計算新的一組（不是滑動視窗）。60 FPS 時約每 5 秒一行，30 FPS 約每 10 秒。
不計算 P95、P99、暖機統計或 ON/OFF 差值。切換 OSD、修改設定、解析度／格式／stride
變更或讀取中斷時，未滿 300 張的資料清除並重新計數；Stop 時不足 300 張也不輸出。
這只控制 log 的彙整，不限制 FPS，也不丟棄影像。

計時從 video-frame event 等待已成功結束、開始取資料及準備／複製擷取 buffer 前的
`acquisition_begin_ns` 起，到 read 返回上層當下的 `steady_clock` 為止。
第 1 步為 `frame.timestamp_ns - acquisition_begin_ns`，包含取得／複製原始 frame、
buffer 必要配置、鎖等待與 backend 管理；不是單獨的 memcpy 時間。
總耗時為 `received_ns - acquisition_begin_ns`，因此包含非 zero-copy 的第一次擷取複製。
公開的 `frame.timestamp_ns` 語意不變，仍記錄原始資料就緒、OSD 前的時間；沒有改動 A/V 同步。

等待下一張 frame 的時間、frame event 就緒前的 driver／FPGA／來源延遲、預覽、錄影與 UI
處理不包含在這份成本中；它不是完整訊號輸入端到端延遲，也不是單獨的 GPU kernel 時間。
OSD 所需上傳、混合、讀回、SDK 的額外複製及取資料後的排程延遲均包含在內。

ON 由 SDK 該張 frame 的 `osd_applied` 確認；與 Apply／Clear 重疊的 read 及無效 timestamp 不輸出，
避免標錯狀態。文字太大或全空白時會加上中文「未疊字」說明，不誤認為正常疊字。
Log 在 release frame 後透過既有 queued signal 回 GUI 輸出，不逐 frame 寫檔或列印。
保持其他設定不變，按 Clear OSD／Apply OSD，就能直接比較 OFF／ON 的耗時。

所有步驟與總耗時使用同一批 300 張樣本。第 2 步合併鎖等待、文字圖檢查／重建及輸出
buffer 準備；其最大值是先計算每張合併時間再取最大，不是把不同張的最大值相加。
若該組含初始化／重建，會另加中文備註。OSD OFF 不執行的步驟會顯示 0。

第 4 步只是 CPU 提交指令，不是 GPU 執行時間；第 5 步可能與 GPU 工作重疊。
第 6 步包含等待 GPU 先前工作完成的時間，不是單純搬回資料所需時間。
每列左邊是平均、右邊是最大，最大值可能發生在不同張，不能把它們相加。
交付上層總耗時還包含驗證、框架記帳與排程等剩餘開銷，不保證等於各步驟總和。

非 zero-copy 模式下，OSD 結果直接貼回 SDK 自有的擷取 buffer，不再複製第二份完整畫面；
第 5 步應顯示 `0.000 / 0.000`。原始 buffer 保留到客戶 release，因此交付前的第 8 步
也為 0（不是不回收，而是延後回收）。仍保留原始 stride，含 padding 的 frame 也按原間距貼回。
zero-copy 模式仍保留獨立輸出及完整複製，不修改 driver buffer。

請先重編 GVFG SDK，再編譯／部署 Viewer，關閉 zero-copy 後重新 Start 測試。
比較最佳化前後時應保持同一個擷取模式。舊版 log 從 `frame.timestamp_ns` 才開始，
未包含初次擷取複製；新版已從複製前起算，不能與舊版總耗時直接比較。
省掉額外複製後，原本與複製重疊的 GPU 等待可能增加，
仍以交付上層總耗時判斷實際改善。

僅內部 timed read 會多讀取時鐘；Console OFF 的一般 read 不啟用分段計時。
未新增 GPU timestamp query、Flush 或等待點，處理流程不變，但計時本身仍有少量 CPU 開銷。
沒有排除首次重建樣本，有初始化備註的那組平均／最大值可能包含初始化成本。

## GVFG Integration

GVFG is imported from a sibling standalone SDK by default:

```text
../../GVFG_Standalone
```

The expected external layout is:

```text
GVFG_Standalone/
  sdk/gvfg/include/gvfg_capture.h
  helpers/gvfg_preview/include/gvfg_preview.h
  build/<config>/bin/gvfg.dll
  build/<config>/bin/gvfg_preview.dll
  build/<config>/lib/gvfg.lib
  build/<config>/lib/gvfg_preview.lib
```

GPU frame-to-buffer conversion is exported directly by `gvfg.dll` through
`gvfg_gpu_convert_to_buffer()` in `gvfg_capture.h`.

GVFG integration is required for Windows builds. Relevant CMake paths:

```text
GVFG_STANDALONE_ROOT=<path-to-GVFG_Standalone>
GVFG_STANDALONE_BUILD_DIR=<path-containing-bin-and-lib>
```

If the direct `GVFG_STANDALONE_BUILD_DIR` does not contain the required files, CMake scans
`GVFG_STANDALONE_ROOT/build/*` and uses the first directory containing
`bin/gvfg.dll` and `lib/gvfg.lib`.

## Packaging

`pack_qt6_viewer.bat` copies:

- `qt6_viewer.exe`
- `gcapture.dll`
- `gdisplay.dll`
- `gvfg.dll`
- `gvfg_preview.dll`
- FFmpeg runtime DLLs when present

Then it runs `windeployqt` and creates a release zip.

## OSD zero-copy 原地寫入實驗（預設關閉）

此開關在 standalone SDK 的 CMake：`GVFG_EXPERIMENTAL_ZERO_COPY_OSD_INPLACE=ON`。
先重建 SDK 的 `gvfg.dll` 與 import library，再重建使用同一套 SDK 的 viewer；
不是只開 viewer 的 console 選項。啟動 zero-copy 並套用 OSD 後，console 的
300 張診斷會標示「直接寫回 driver buffer」或「頁面檢查未通過，退回獨立輸出」。
前者應不再有額外完整畫面複製，但仍有 GPU 上傳、等待、讀回與 CPU 貼回文字區域。

僅供本機實驗，不可出貨：`VirtualQuery` 的可寫檢查不能保證 DMA／driver 不會同時
覆寫畫面，也不能防止檢查後 mapping 改變，仍可能破圖或程式崩潰。未通過初次檢查
會退回原路徑；寫回前第二次檢查失敗則回傳錯誤而不寫入。不修改既有 driver release
規則。測完將此 SDK 選項改回 OFF 並重建 SDK，才會恢復正式路徑。
