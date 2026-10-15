# Capture Backend 邊界

Viewer 目前維護兩條擷取路徑。兩者刻意不共用 backend 實作，避免把
GVFG 與 DirectShow 的生命週期、資源 ownership 和相依性混在一起。

## GVFG Direct

`apps/qt6_viewer/gvfg` 負責 viewer 端的 GVFG 整合。它只使用下列由
`GVFG_STANDALONE_ROOT` 與 `GVFG_STANDALONE_BUILD_DIR` 指定的外部
`GVFG_Standalone` 產出：

- `gvfg`：裝置與 frame 擷取
- `gvfg_preview`：D3D preview
- `gvfg_audio_playback`：WASAPI 音訊播放

GVFG 擷取不可繞道使用 `sdk/gcapture`。由 GVFG SDK 擁有的 frame 或
audio packet，必須在對應的 release 呼叫之前完成使用，或先複製到
應用程式自己擁有的記憶體。

`apps/qt6_viewer/app/mainwindow_gvfg.cpp` 負責 MainWindow 端的 GVFG
session 操作，包括開始擷取、開始／停止錄影，以及 GVFG 專屬狀態回報。
共用的 `mainwindow_capture.cpp` 只負責分派這些操作，並維護共用的 UI
與 session 狀態。

## DirectShow

`sdk/gcapture` 負責 DirectShow 擷取路徑及其公開 `gcap_*` API。
DirectShow graph、signal probe、自訂／RAW renderer 與 DirectShow audio
monitoring 都封裝在這個模組內。這裡描述的 RAW 與 audio 都只屬於
DirectShow 路徑，不代表 GVFG 的 RAW frame 或 audio packet 會經過
`sdk/gcapture`。

Viewer 可以呼叫公開的 `gcap_*` API，但不可 include DirectShow provider
的私有 header，也不可直接操作 DirectShow graph。

`apps/qt6_viewer/app/mainwindow_dshow.cpp` 透過公開 `gcap_*` API，負責
MainWindow 端的 DirectShow session 操作，包括開始擷取、開始／停止錄影、
audio monitoring，以及 DirectShow 錄影結果回報。共用的
`mainwindow_capture.cpp` 只負責分派，不直接實作 backend 擷取或錄影設定。

DirectShow 的錄影控制位於 `DShowProvider`，但實際編碼與封裝使用下方的
共用 `capture_ffmpeg_recorder`。這不表示 GVFG 錄影會進入 DirectShow
provider；兩條路徑只共用 recorder helper。

## 共用錄影 Helper

`capture_ffmpeg_recorder` 是 DirectShow SDK 與 GVFG viewer 明確共用的
錄影邊界。Viewer 會連結這個 target，不會直接編譯
`sdk/gcapture/src/providers` 內的檔案，也不會把該私有目錄加入 include path。
其實作位於 backend-neutral 的 `sdk/recording`，不依賴 `gcapture.h`、
`gcap_pixfmt_t` 或任何 `gcap_*` API。

Capture ownership 與 preview 仍由各 backend 自己負責；只有已複製完成的
video frame／audio data 可以交給共用 recorder。

換句話說：

- GVFG video、RAW、audio、preview 都直接走 `gvfg_*` 與 GVFG helper。
- DirectShow video、RAW、audio、preview 都由 `sdk/gcapture` 的 DirectShow
  實作負責。
- 兩者只在「已複製完成、準備編碼的資料」這一層共用
  `capture_ffmpeg_recorder`。

## 後續修改原則

新增功能與修正應優先放在 GVFG Direct 路徑。DirectShow 維護必須留在
`gcapture` 邊界內，不可在 GVFG 程式碼與 DirectShow 內部實作之間新增相依。
