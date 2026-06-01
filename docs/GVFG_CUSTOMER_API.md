# GVFG 客戶 API 使用說明

這份文件說明 VFG100 / GVFG capture SDK 對外給客戶使用的 C API。API 名稱統一使用 `gvfg_` prefix；`XDMA` 只代表目前底層 driver/backend。

目前 API 定義在：

```text
sdk/gvfg/include/gvfg_capture.h
```

`gvfg` 是獨立的 GVFG capture SDK。它和 `gcapture` 分開，所以只需要 VFG100 / GVFG 功能的客戶，不需要看到或使用 DirectShow / Media Foundation 那一批 API。

後續 XDMA driver/backend 還會持續開發，所以這份文件先以中文維護。等 API 和流程穩定後，再整理英文版給客戶。

## 要提供給客戶的檔案

Windows app 使用 GVFG capture 時，執行檔旁邊需要放：

```text
gvfg.dll
gvendor.dll
```

開發時需要 include：

```text
sdk/gvfg/include
```

客戶端只需要 include：

```c
#include <gvfg_capture.h>
```

下面這些是 SDK 內部檔案，不是客戶 API：

```text
sdk/gvendor/include/gvendor.h
sdk/gdriver_shared/include
```

link：

```text
gvfg.lib
```

最小 Qt UI 範例：

```text
samples/gvfg_qt_preview
```

這個 sample 只示範 GVFG API，不依賴原本 `apps/qt6_viewer`。

CMake build option：

```text
BUILD_GVFG_SDK=ON
```

如果要開 XDMA flow debug log：

```text
GVENDOR_XDMA_DEBUG_LOG=ON
```

debug log 目前會走 `OutputDebugStringA`，viewer 也會把部分 log 顯示到 debug panel / log file。

## 整體架構

目前 XDMA 分成兩層：

```text
客戶 App
  |
  | include gvfg_capture.h
  | link gvfg.lib
  v
gvfg.dll
  |
  | 對外提供簡單 XDMA capture API
  | 管理 preview、callback、runtime info
  v
gvendor.dll
  |
  | 低階 XDMA driver access
  | enumerate / open / c2h read / event / signal
  v
XDMA Windows driver
```

目前 preview path：

```text
XDMA C2H 讀到 YUY2 frame
-> gvendor 收 driver buffer
-> gvfg 拿 frame
-> D3D SharedScenePipeline
-> GPU shader 做 YUY2 -> RGB
-> D3D swapchain present 到客戶提供的 HWND
```

重點：

- 主要 preview 是 D3D swapchain 直接畫到 `HWND`。
- frame callback 目前主要給 snapshot、UI 狀態、或 fallback preview 用。
- callback 不是主要零拷貝 preview path。

## 建議使用流程

一般客戶程式照這個順序：

```text
gvfg_enumerate_devices
-> gvfg_create
-> gvfg_set_callbacks
-> gvfg_open
-> gvfg_start
-> capture running
-> gvfg_get_signal_status / gvfg_get_runtime_info
-> gvfg_stop
-> gvfg_destroy
```

簡單說：

1. 先列出 XDMA 裝置。
2. 建立一個 `gvfg_handle`。
3. 設 callback。
4. open device。
5. start stream。
6. stop / destroy。

如果客戶想使用 SDK 內建 D3D preview helper，才需要多呼叫：

```text
gvfg_set_preview
gvfg_get_preview_info
```

如果客戶要自己畫畫面，就不要呼叫 `gvfg_set_preview()`，直接使用 frame callback 或未來的 GPU texture API。

## 型別說明

### gvfg_handle

```c
typedef struct gvfg_handle_t *gvfg_handle;
```

這是 SDK session handle。

客戶不要直接存取裡面的內容，只能透過 API 操作。

生命週期：

```text
gvfg_create
-> 使用 handle
-> gvfg_destroy
```

## gvfg_status_t

大部分 API 都會回傳這個 status。

```c
typedef enum
{
    GVFG_OK = 0,
    GVFG_EINVAL = -1,
    GVFG_ENODEV = -2,
    GVFG_ESTATE = -3,
    GVFG_EIO = -4,
    GVFG_ENOTSUP = -5,
    GVFG_ETIMEOUT = -6
} gvfg_status_t;
```

| 值 | 意義 |
| --- | --- |
| `GVFG_OK` | 成功 |
| `GVFG_EINVAL` | 參數錯誤，例如傳入 null pointer |
| `GVFG_ENODEV` | 找不到裝置，或目前沒有有效 signal |
| `GVFG_ESTATE` | 狀態錯誤，例如還沒 open 就 start |
| `GVFG_EIO` | driver I/O 錯誤 |
| `GVFG_ENOTSUP` | 功能尚未支援，或 preview pipeline 無法建立 |
| `GVFG_ETIMEOUT` | 等待 frame / event timeout |

如果要把錯誤碼轉成人能看的字串，用：

```c
gvfg_strerror(status)
```

## gvfg_preview_bitdepth_t

preview swapchain bit-depth 設定。

```c
typedef enum
{
    GVFG_PREVIEW_BITDEPTH_AUTO = 0,
    GVFG_PREVIEW_BITDEPTH_10BIT = 10,
    GVFG_PREVIEW_BITDEPTH_8BIT = 8
} gvfg_preview_bitdepth_t;
```

| 值 | 意義 |
| --- | --- |
| `GVFG_PREVIEW_BITDEPTH_AUTO` | 讓 SDK 自己判斷 |
| `GVFG_PREVIEW_BITDEPTH_10BIT` | 要求 10-bit swapchain，如果環境支援 |
| `GVFG_PREVIEW_BITDEPTH_8BIT` | 強制 8-bit swapchain |

目前 XDMA input 是 8-bit YUY2，所以通常會建立 8-bit BGRA swapchain。

## gvfg_device_info_t

列舉裝置時回傳的裝置資料。

```c
typedef struct
{
    int index;
    char name[128];
} gvfg_device_info_t;
```

| 欄位 | 意義 |
| --- | --- |
| `index` | 裝置 index，之後給 `gvfg_open()` 使用 |
| `name` | 顯示名稱，例如 `XDMA Capture Device 0` |

## gvfg_preview_desc_t

preview 設定。

```c
typedef struct
{
    void *hwnd;
    int enable_preview;
    int swapchain_bitdepth;
} gvfg_preview_desc_t;
```

| 欄位 | 意義 |
| --- | --- |
| `hwnd` | 要讓 SDK 畫 preview 的 Win32 `HWND` |
| `enable_preview` | `1` 啟用 preview，`0` 關閉 preview |
| `swapchain_bitdepth` | `gvfg_preview_bitdepth_t` 的值 |

注意：

- `hwnd` 必須是 native Win32 window handle。
- capture running 的時候，這個 window 必須還活著。
- 如果 app 是 Qt / WinForms / WPF，要拿到真正可用的 native HWND。

## gvfg_signal_status_t

目前 input signal 狀態。

```c
typedef struct
{
    int width;
    int height;
    double fps;
    int bit_depth;
    char pixel_format[32];
} gvfg_signal_status_t;
```

| 欄位 | 意義 |
| --- | --- |
| `width` | input 寬度 |
| `height` | input 高度 |
| `fps` | input FPS，SDK 已經換算好，例如 `29.97` |
| `bit_depth` | input bit depth |
| `pixel_format` | pixel format 字串，目前預期是 `YUY2` |


## gvfg_runtime_info_t

runtime debug/status 資訊。

```c
typedef struct
{
    gvfg_signal_status_t input_signal;
    double capture_fps;
    uint64_t delivered_frames;
} gvfg_runtime_info_t;
```

| 欄位 | 意義 |
| --- | --- |
| `input_signal` | 目前 input signal 狀態 |
| `capture_fps` | SDK capture runtime 測到的 FPS |
| `delivered_frames` | SDK 已交付/處理的 frame 數 |

這個 struct 適合顯示在：

- status bar
- debug panel
- support log
- 客戶回報問題時的診斷資訊

注意：

`gvfg_runtime_info_t` 是 core capture runtime 狀態，不描述 preview/render。  
如果要查 SDK 內建 D3D preview helper 的 render/swapchain 狀態，請用 `gvfg_get_preview_info()`。

## gvfg_preview_info_t

optional preview helper 的狀態。

如果客戶自己畫畫面，不使用 `gvfg_set_preview()`，這個 struct 通常不需要使用。

```c
typedef struct
{
    int enabled;
    int active;
    int width;
    int height;
    int swapchain_bitdepth;
    int swapchain_10bit;
    char render_path[128];
    char backbuffer_format[64];
} gvfg_preview_info_t;
```

| 欄位 | 意義 |
| --- | --- |
| `enabled` | app 是否有啟用 SDK preview helper |
| `active` | preview pipeline / swapchain 是否已建立 |
| `width` | preview swapchain 寬度 |
| `height` | preview swapchain 高度 |
| `swapchain_bitdepth` | 目前/要求的 swapchain bit depth |
| `swapchain_10bit` | 目前 swapchain 是否是 10-bit |
| `render_path` | SDK preview helper 的 render path |
| `backbuffer_format` | D3D swapchain backbuffer format |

這些資訊只描述 SDK 內建 preview helper，不代表客戶 app 自己的 rendering pipeline。

## gvfg_frame_t

frame callback 的 payload。

```c
typedef struct
{
    const void *data;
    int stride;
    int width;
    int height;
    uint64_t pts_ns;
    uint64_t frame_id;
} gvfg_frame_t;
```

| 欄位 | 意義 |
| --- | --- |
| `data` | pixel data pointer |
| `stride` | 每一列 bytes |
| `width` | frame 寬度 |
| `height` | frame 高度 |
| `pts_ns` | timestamp，單位 ns |
| `frame_id` | frame counter |

重要：

- `data` 只在 callback 當下有效。
- callback return 後不能再使用這個 pointer。
- 如果客戶要存圖、做 snapshot、或丟給其他 thread，要自己 copy 一份。
- 目前 callback 輸出是 8-bit BGRA-compatible 格式，在 Windows/Qt 可用 `QImage::Format_ARGB32` 接。
- callback 由 SDK worker thread 呼叫，不是在 UI thread。

UI 程式要注意：

```text
callback thread
-> copy frame
-> post / invoke 到 UI thread
-> UI thread 更新畫面
```

不要直接在 callback 裡操作 UI control。

## Callback 型別

```c
typedef void (*gvfg_on_frame_cb)(const gvfg_frame_t *frame, void *user);
typedef void (*gvfg_on_error_cb)(gvfg_status_t status, const char *message, void *user);
```

### gvfg_on_frame_cb

SDK 有 frame readback / fallback frame 時呼叫。

用途：

- snapshot
- UI preview fallback
- 確認 XDMA frame 有進來
- 客戶自己後處理

目前不建議把它當作主要高效 preview path。主要 preview 仍然是 D3D swapchain。

### gvfg_on_error_cb

SDK 有錯誤或警告訊息時呼叫。

用途：

- 顯示錯誤訊息
- 寫 log
- debug / support

## Function 說明

## gvfg_enumerate_devices

```c
enum { GVFG_MAX_DEVICES = 16 };

int gvfg_enumerate_devices(gvfg_device_info_t *out_devices, int max_devices);
```

列出目前系統上的 GVFG capture device。

### 參數

| 參數 | 說明 |
| --- | --- |
| `out_devices` | 輸出 array。如果只想取得數量，可傳 `NULL` |
| `max_devices` | `out_devices` 最多可寫入幾個；SDK 上限是 `GVFG_MAX_DEVICES` / 16 |

### 回傳值

| 回傳 | 意義 |
| --- | --- |
| `> 0` | 找到的裝置數量，或實際寫入數量 |
| `0` | 沒找到裝置 |
| `< 0` | enumerate 失敗 |

### 範例

```c
gvfg_device_info_t devices[GVFG_MAX_DEVICES] = {0};
int count = gvfg_enumerate_devices(devices, GVFG_MAX_DEVICES);

for (int i = 0; i < count; ++i) {
    printf("device %d: %s\n", devices[i].index, devices[i].name);
}
```

## gvfg_create

```c
gvfg_status_t gvfg_create(gvfg_handle *out_handle);
```

建立一個 GVFG capture session。

### 參數

| 參數 | 說明 |
| --- | --- |
| `out_handle` | 輸出 handle，不可為 `NULL` |

### 回傳值

成功回傳 `GVFG_OK`。

### 注意

這時候只是建立 session，還沒有 open device，也還沒有 start stream。

用完必須呼叫：

```c
gvfg_destroy(handle);
```

## gvfg_destroy

```c
gvfg_status_t gvfg_destroy(gvfg_handle handle);
```

釋放 GVFG capture session。

### 參數

| 參數 | 說明 |
| --- | --- |
| `handle` | `gvfg_create()` 回傳的 handle |

### 回傳值

目前固定回傳 `GVFG_OK`。

### 注意

如果 capture 還在跑，destroy 時會自動 stop / close。

建議正常流程還是明確呼叫：

```c
gvfg_stop(handle);
gvfg_destroy(handle);
```

## gvfg_set_callbacks

```c
gvfg_status_t gvfg_set_callbacks(gvfg_handle handle,
                                   gvfg_on_frame_cb on_frame,
                                   gvfg_on_error_cb on_error,
                                   void *user);
```

設定 frame callback 和 error callback。

### 參數

| 參數 | 說明 |
| --- | --- |
| `handle` | session handle |
| `on_frame` | frame callback，可為 `NULL` |
| `on_error` | error callback，可為 `NULL` |
| `user` | 客戶自訂 pointer，callback 時會原樣帶回 |

### 回傳值

成功回傳 `GVFG_OK`。

### 注意

callback 是從 SDK worker thread 呼叫。

如果是 GUI app，不要在 callback 裡直接更新 UI，請 marshal 到 UI thread。

## gvfg_set_preview

```c
gvfg_status_t gvfg_set_preview(gvfg_handle handle, const gvfg_preview_desc_t *desc);
```

設定 optional D3D preview helper 要畫到哪個 window。

這個 API 不是 core capture 必要流程。  
如果客戶自己處理顯示，就不需要呼叫這個 function。

### 參數

| 參數 | 說明 |
| --- | --- |
| `handle` | session handle |
| `desc` | preview 設定，不可為 `NULL` |

### 回傳值

成功回傳 `GVFG_OK`。

### 範例

```c
gvfg_preview_desc_t preview = {0};
preview.hwnd = (void *)hwnd;
preview.enable_preview = 1;
preview.swapchain_bitdepth = GVFG_PREVIEW_BITDEPTH_AUTO;

gvfg_set_preview(handle, &preview);
```

### 注意

一般建議在 `gvfg_start()` 前呼叫。

目前也可在 running 中呼叫，用來更新 preview target 或 bit-depth request。

## gvfg_open

```c
gvfg_status_t gvfg_open(gvfg_handle handle, int device_index);
```

開啟指定的 GVFG device。

### 參數

| 參數 | 說明 |
| --- | --- |
| `handle` | session handle |
| `device_index` | `gvfg_enumerate_devices()` 得到的 device index |

### 回傳值

成功回傳 `GVFG_OK`。

### 目前行為

`gvfg_open()` 目前會：

1. open XDMA device。
2. 設定 SDI input。
3. 讀取目前 signal width / height / fps。

stream 的解析度會依照 signal 自動調整。  
例如 log 看到：

```text
signal: width=1280 height=720 configured=1920x1080
configure: effective ... 1280x720
```

代表一開始預設可能是 1920x1080，但 driver 讀到實際 signal 是 1280x720，所以最後 stream 會用 1280x720。

## gvfg_start

```c
gvfg_status_t gvfg_start(gvfg_handle handle);
```

開始 GVFG streaming。

### 參數

| 參數 | 說明 |
| --- | --- |
| `handle` | 已經 open 的 session handle |

### 回傳值

成功回傳 `GVFG_OK`。

### 目前行為

`gvfg_start()` 會：

1. configure stream。
2. 建立 D3D preview pipeline。
3. start XDMA event thread。
4. start XDMA data thread。
5. 從 C2H 讀 frame。
6. 把 YUY2 frame 丟給 GPU shader 轉 RGB。
7. present 到 preview HWND。

正常 log 會看到：

```text
[GVendor][XDMA] start: threads launched
[GVendor][XDMA] publish_frame: id=1 ...
[GVendor][XDMA] wait_frame: deliver id=1 ...
[SharedScene] preview render path ...
```

## gvfg_stop

```c
gvfg_status_t gvfg_stop(gvfg_handle handle);
```

停止 GVFG streaming。

### 參數

| 參數 | 說明 |
| --- | --- |
| `handle` | session handle |

### 回傳值

成功回傳 `GVFG_OK`。

### 注意

即使目前沒有 running，也可以呼叫。  
SDK 會做安全檢查。

## gvfg_get_signal_status

```c
gvfg_status_t gvfg_get_signal_status(gvfg_handle handle,
                                       gvfg_signal_status_t *out_status);
```

取得目前 input signal 狀態。

### 參數

| 參數 | 說明 |
| --- | --- |
| `handle` | session handle |
| `out_status` | 輸出 signal status，不可為 `NULL` |

### 回傳值

如果有有效 width / height，回傳 `GVFG_OK`。  
否則回傳錯誤狀態。

### 範例

```c
gvfg_signal_status_t sig = {0};
gvfg_status_t st = gvfg_get_signal_status(handle, &sig);

if (st == GVFG_OK) {
    printf("%dx%d %.2f fps %s\n",
           sig.width,
           sig.height,
           sig.fps,
           sig.pixel_format);
}
```

## gvfg_get_runtime_info

```c
gvfg_status_t gvfg_get_runtime_info(gvfg_handle handle,
                                      gvfg_runtime_info_t *out_info);
```

取得 core capture runtime 診斷資訊。

### 參數

| 參數 | 說明 |
| --- | --- |
| `handle` | session handle |
| `out_info` | 輸出 runtime info，不可為 `NULL` |

### 回傳值

成功回傳 `GVFG_OK`。

### 建議用途

可以用來顯示：

- 目前解析度
- runtime FPS
- source format
- backend / frame source
- delivered frame count

例如 viewer status bar 可以顯示：

```text
Backend: GVFG | Source: XDMA C2H | Input 1280x720 29.97fps YUY2 | Runtime 29.97fps | Frames 120
```

## gvfg_get_preview_info

```c
gvfg_status_t gvfg_get_preview_info(gvfg_handle handle,
                                      gvfg_preview_info_t *out_info);
```

取得 optional D3D preview helper 狀態。

### 參數

| 參數 | 說明 |
| --- | --- |
| `handle` | session handle |
| `out_info` | 輸出 preview info，不可為 `NULL` |

### 回傳值

成功回傳 `GVFG_OK`。

### 建議用途

只有在客戶使用 `gvfg_set_preview()` 時才需要呼叫。

可以用來顯示：

- SDK preview 是否啟用
- swapchain 是否建立
- preview size
- backbuffer format
- SDK preview shader/render path

如果客戶自己畫畫面，render 狀態應該由客戶自己的 app 顯示，不應該依賴這個 function。

## gvfg_strerror

```c
const char *gvfg_strerror(gvfg_status_t status);
```

把 `gvfg_status_t` 轉成字串。

### 參數

| 參數 | 說明 |
| --- | --- |
| `status` | API 回傳的 status |

### 回傳值

回傳 static string pointer。  
客戶不需要也不能 free。

### 範例

```c
gvfg_status_t st = gvfg_start(handle);
if (st != GVFG_OK) {
    printf("gvfg_start failed: %s\n", gvfg_strerror(st));
}
```

## 最小使用範例

```c
#include "gvfg_capture.h"
#include <stdio.h>

static void on_frame(const gvfg_frame_t *frame, void *user)
{
    (void)user;

    if (!frame || !frame->data)
        return;

    printf("frame %llu: %dx%d stride=%d\n",
           (unsigned long long)frame->frame_id,
           frame->width,
           frame->height,
           frame->stride);

    /*
      如果要存圖或交給其他 thread，這裡要 copy frame->data。
      callback return 後 frame->data 就不能再使用。
    */
}

static void on_error(gvfg_status_t status, const char *message, void *user)
{
    (void)user;
    printf("gvfg message %d: %s\n",
           (int)status,
           message ? message : "");
}

int start_xdma_preview(void *hwnd)
{
    gvfg_device_info_t devices[GVFG_MAX_DEVICES] = {0};
    int device_count = gvfg_enumerate_devices(devices, GVFG_MAX_DEVICES);

    if (device_count <= 0) {
        printf("No GVFG device found\n");
        return -1;
    }

    gvfg_handle handle = NULL;
    gvfg_status_t st = gvfg_create(&handle);

    if (st != GVFG_OK) {
        printf("gvfg_create failed: %s\n", gvfg_strerror(st));
        return -1;
    }

    gvfg_set_callbacks(handle, on_frame, on_error, NULL);

    /*
      Optional:
      如果客戶要使用 SDK 內建 D3D preview helper，才需要設定 preview HWND。
      如果客戶自己畫畫面，可以整段省略。
    */
    if (hwnd) {
        gvfg_preview_desc_t preview = {0};
        preview.hwnd = hwnd;
        preview.enable_preview = 1;
        preview.swapchain_bitdepth = GVFG_PREVIEW_BITDEPTH_AUTO;
        gvfg_set_preview(handle, &preview);
    }

    st = gvfg_open(handle, devices[0].index);
    if (st != GVFG_OK) {
        printf("gvfg_open failed: %s\n", gvfg_strerror(st));
        gvfg_destroy(handle);
        return -1;
    }

    st = gvfg_start(handle);
    if (st != GVFG_OK) {
        printf("gvfg_start failed: %s\n", gvfg_strerror(st));
        gvfg_destroy(handle);
        return -1;
    }

    /*
      注意：handle 必須保存起來。
      capture running 期間不能 destroy。

      停止時：

      gvfg_stop(handle);
      gvfg_destroy(handle);
    */

    return 0;
}
```

## Qt 使用注意

Qt viewer 目前使用 optional preview helper：

```text
previewWindow_->previewHwnd()
-> 傳給 gvfg_set_preview()
-> gvfg 用 D3D swapchain 畫到 previewHost
```

Qt callback：

```text
gvfg worker thread
-> emit frameReady(QImage copy)
-> Qt queued connection
-> UI thread 更新 snapshot/狀態
```

XDMA 模式下，Qt 不應該再用 QLabel overlay 蓋住 D3D preview。  
frame callback 可以保留給 snapshot、初始解析度調整、debug。

如果客戶不用 SDK preview helper，Qt app 可以自己用 callback frame 或自家的 GPU pipeline 畫畫面。

## 目前限制

目前版本限制：

- input format 目前固定走 YUY2。
- `gvfg` 還沒有對外提供 recording API。
- 目前沒有 expose 手動選 input source 的 API，內部先固定 SDI。
- `gvfg_set_preview()` 是 optional helper，不是 core capture 必要 API。
- frame callback 不是主要 SDK preview path。
- callback data pointer 只在 callback 期間有效。
- callback thread 不是 UI thread。

## 正常 log 範例

啟動成功時，大致會看到：

```text
[GVendor][XDMA] enumerate: done count=1
[GVendor][XDMA] open_device: done
[GVendor][XDMA] signal: width=1280 height=720
[GVendor][XDMA] configure: effective ... 1280x720 ... frame_bytes=1843200
[SharedScene] preview swapchain created ...
[GVendor][XDMA] start: threads launched
[GVendor][XDMA] publish_frame: id=1 ...
[GVendor][XDMA] wait_frame: deliver id=1 ...
[SharedScene] preview render path ...
```

代表：

```text
device 找到
-> device open 成功
-> signal 讀到解析度
-> stream configure 成功
-> D3D preview 建立成功
-> XDMA frame 有進來
-> preview shader path 有跑
```

## 問題判斷

### 有 enumerate，但 start 失敗

可能原因：

- device 被其他程式占用
- driver subdevice open 失敗
- event channel open 失敗
- stream configure 失敗

### 有 start，但沒有 frame

log 如果沒有：

```text
publish_frame
wait_frame: deliver
```

可能原因：

- XDMA interrupt event 沒來
- c2h read 沒資料
- FPGA/driver capture enable 沒生效
- input signal 或 DMA path 有問題

### 有 frame，但 preview 黑畫面

如果有：

```text
publish_frame
wait_frame: deliver
```

但沒有：

```text
[SharedScene] preview swapchain created
[SharedScene] preview render path
```

可能是：

- 傳進來的 `HWND` 無效
- preview window 還沒建立 native handle
- D3D device / swapchain 建立失敗

如果兩者都有，但畫面仍異常，要查：

- YUY2 shader 轉換
- source stride
- swapchain present
- window 被 UI overlay 蓋住

## 後續可能會新增的 API

目前先保留未來擴充方向：

- 選 input source，例如 SDI / HDMI
- 手動指定解析度 / fps / pixel format
- recording API
- snapshot / raw export API
- 取得 driver / firmware / card info
- 設定 debug log callback
- expose GPU adapter selection
- expose zero-copy / GPU texture callback
