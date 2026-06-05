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
None. Internal XDMA backend/gdriver headers are not distributed.
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
GVFG_XDMA_DEBUG_LOG=ON
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
gvfg.dll
  |
  | 低階 XDMA driver access
  | enumerate / open / c2h read / event / signal
  v
XDMA Windows driver
```

目前 preview path：

```text
XDMA C2H 讀到 YUY2 frame
-> GVFG internal XDMA backend 收 driver buffer
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
-> gvfg_set_frame_callback_interval (optional)
-> gvfg_set_event_callback (optional)
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
4. 視需求設定 callback 速率。
5. 視需求設定 event callback。
6. open device。
7. start stream。
8. stop / destroy。

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


### Customer buffer vs FPGA signal

The customer API separates the delivered buffer format from the FPGA signal
register values.

```c
typedef struct
{
    uint32_t valid_mask;            /* FPGA register read-valid mask: bit0=0x0c, bit1=0x18, bit2=0x1c, bit3=0x180. */
    int width_valid;                /* Non-zero when FPGA 0x10 read succeeded. */
    int height_valid;               /* Non-zero when FPGA 0x14 read succeeded. */
    uint32_t width_raw;             /* Raw FPGA 0x10 width register. */
    uint32_t height_raw;            /* Raw FPGA 0x14 height register. */
    uint32_t video_format_raw;      /* Raw FPGA 0x0c value. */
    int video_format_valid;
    int video_format_code;       /* 0=yuv422, 1=rgb, 2=yuv444, 3=yuv420 */
    char video_format[16];

    uint32_t frame_rate_raw;        /* Raw FPGA 0x18 value. */
    int frame_rate_valid;
    int frame_rate_code;         /* FPGA 0x18 low nibble. */
    char frame_rate_bits[5];     /* 4-bit binary text, for example "0110". */
    char frame_rate_name[16];    /* None, 23.98, 24, 47.95, ..., or "--" for unsupported codes. */

    uint32_t bit_depth_raw;         /* Raw FPGA 0x1c value. */
    int bit_depth_valid;
    int bit_depth;               /* FPGA signal bit depth: 8 or 10 when valid. */

    uint32_t status_raw;            /* Raw FPGA 0x180 value. */
    int status_valid;
    int sdi_locked;              /* FPGA 0x180 bit0 */
    int sdi_ddr_ok;              /* FPGA 0x180 bit1 */
    int hdmi_locked;             /* FPGA 0x180 bit2 */
    int hdmi_ddr_ok;             /* FPGA 0x180 bit3 */
} gvfg_fpga_signal_status_t;

typedef struct
{
    int width;                       /* Signal width in pixels, from FPGA when available. */
    int height;                      /* Signal height in pixels, from FPGA when available. */
    double fps;                      /* Legacy field; GVFG leaves this 0. Use fpga.frame_rate_* instead. */
    int bit_depth;                   /* Legacy delivered-buffer field. Prefer runtime delivered_frame. */
    char pixel_format[32];           /* Legacy delivered-buffer field. Prefer runtime delivered_frame. */
    gvfg_fpga_signal_status_t fpga;  /* Raw/decoded FPGA signal metadata. */
} gvfg_signal_status_t;
```

Use `gvfg_signal_status_t::fpga` for FPGA-reported raw/decoded signal data.
For delivered frame format, prefer `gvfg_runtime_info_t::delivered_frame`.

For example, if FPGA reports `video_format=YUV422` and `bit_depth=10`, but
`pixel_format=YUY2` and `bit_depth=8`, the input signal is reported as YUV422
10-bit by FPGA, but the customer buffer is still YUY2 8-bit packing. Treat the
buffer as true YUV422 10-bit only when `pixel_format` is `Y210`.

FPGA frame-rate code table:

| Bits | Name |
| --- | --- |
| `0000` | None |
| `0010` | 23.98 |
| `0011` | 24 |
| `0100` | 47.95 |
| `0101` | 25 |
| `0110` | 29.97 |
| `0111` | 30 |
| `1000` | 48 |
| `1001` | 50 |
| `1010` | 59.94 |
| `1011` | 60 |

All other codes, including `0001`, are reported as `--`.

## gvfg_runtime_info_t

runtime debug/status 資訊。

```c
typedef struct
{
    int width;
    int height;
    int bit_depth;
    char pixel_format[32];
} gvfg_delivered_frame_info_t;

typedef struct
{
    gvfg_signal_status_t input_signal;
    gvfg_delivered_frame_info_t delivered_frame;
    double capture_fps;
    uint64_t delivered_frames;
} gvfg_runtime_info_t;
```

| 欄位 | 意義 |
| --- | --- |
| `input_signal` | 目前 input signal 狀態 |
| `delivered_frame` | Frame buffer delivered by gvfg.dll to the app callback: width, height, pixel format, bit depth |
| `capture_fps` | SDK capture runtime 測到的 FPS |
| `delivered_frames` | SDK 已交付/處理的 frame 數 |

Source ownership:

- FPGA reported signal: use `input_signal.fpga.*`.
- Delivered frame buffer: use `delivered_frame.*`.
- App/SDK runtime counters: use `capture_fps` and `delivered_frames`.

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

## gvfg_set_frame_callback_interval

```c
gvfg_status_t gvfg_set_frame_callback_interval(gvfg_handle handle,
                                                uint32_t frame_interval);
```

設定 frame callback 的輸出頻率。

### 參數

| 參數 | 說明 |
| --- | --- |
| `handle` | session handle |
| `frame_interval` | `0` 或 `1` 代表每張 frame 都 callback；`N > 1` 代表每 N 張 backend frame callback 一張 |

### 行為

- 只影響 `on_frame` callback。
- 不影響 SDK 內建 preview render。
- 不影響實際採集速率。
- 不影響 `gvfg_get_runtime_info()` 的 delivered frame 統計。

如果客戶要自己靠 callback 畫 preview，建議設 `1`。如果 callback 只拿來做 snapshot、log、或低頻 UI 狀態，可設 `6`、`30` 等較大的值。

## gvfg_set_event_callback

```c
typedef enum
{
    GVFG_EVENT_VIDEO_IRQ = 1,
    GVFG_EVENT_PLUG_IN = 2,
    GVFG_EVENT_PLUG_OUT = 3,
    GVFG_EVENT_CAPTURE_PAUSED = 4,
    GVFG_EVENT_CAPTURE_RESUMED = 5
} gvfg_event_type_t;

typedef struct
{
    gvfg_event_type_t type;
    uint32_t channel;
    uint32_t irq_bit;
    uint32_t irq_mask;
    uint64_t timestamp_ns;
} gvfg_event_t;

typedef void (*gvfg_on_event_cb)(const gvfg_event_t *event, void *user);

gvfg_status_t gvfg_set_event_callback(gvfg_handle handle,
                                      gvfg_on_event_cb on_event,
                                      void *user,
                                      uint32_t event_mask);
```

註冊 capture event callback，用來通知上層 plug in/out 或 SDK 自動 pause/resume 狀態。

### event mask

| mask | 說明 |
| --- | --- |
| `GVFG_EVENT_MASK_PLUG_IN` | 收到 FPGA PLUG_IN IRQ |
| `GVFG_EVENT_MASK_PLUG_OUT` | 收到 FPGA PLUG_OUT IRQ |
| `GVFG_EVENT_MASK_CAPTURE_PAUSED` | SDK 已因 plug out 完成自動 pause |
| `GVFG_EVENT_MASK_CAPTURE_RESUMED` | SDK 已因 plug in 完成自動 resume |
| `GVFG_EVENT_MASK_VIDEO_IRQ` | 收到 video IRQ，通常每張 frame 一次 |
| `GVFG_EVENT_MASK_DEFAULT` | 預設 hotplug/capture state，不包含 video IRQ |
| `GVFG_EVENT_MASK_ALL` | 全部事件，包含 video IRQ |

`event_mask` 傳 `0` 時使用 `GVFG_EVENT_MASK_DEFAULT`。一般客戶建議用 default；`VIDEO_IRQ` 頻率高，建議只在 debug 或明確需要時打開。

### 範例

```c
static void on_event(const gvfg_event_t *event, void *user)
{
    switch (event->type) {
    case GVFG_EVENT_PLUG_OUT:
        /* UI 顯示 signal disconnected */
        break;
    case GVFG_EVENT_PLUG_IN:
        /* UI 顯示 signal connected */
        break;
    case GVFG_EVENT_CAPTURE_PAUSED:
        /* SDK 已自動 pause capture */
        break;
    case GVFG_EVENT_CAPTURE_RESUMED:
        /* SDK 已自動 resume capture */
        break;
    default:
        break;
    }
}

gvfg_set_event_callback(handle, on_event, user, GVFG_EVENT_MASK_DEFAULT);
```

注意：event callback 是通知用途。熱插拔時的 stop/start/pause/resume 邏輯仍由 SDK 自己處理，客戶不需要在 event callback 裡重啟 capture。

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
[GVFG][XDMA] start: threads launched
[GVFG][XDMA] publish_frame: id=1 ...
[GVFG][XDMA] wait_frame: deliver id=1 ...
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
    printf("Signal legacy: %dx%d\n",
           sig.width,
           sig.height);

    printf("FPGA raw\n"
           "  valid_mask=0x%08x\n"
           "  width_valid=%d width_raw=%u\n"
           "  height_valid=%d height_raw=%u\n"
           "  video_format_raw=0x%08x\n"
           "  frame_rate_raw=0x%08x\n"
           "  bit_depth_raw=0x%08x\n"
           "  status_raw=0x%08x\n",
           sig.fpga.valid_mask,
           sig.fpga.width_valid,
           sig.fpga.width_raw,
           sig.fpga.height_valid,
           sig.fpga.height_raw,
           sig.fpga.video_format_raw,
           sig.fpga.frame_rate_raw,
           sig.fpga.bit_depth_raw,
           sig.fpga.status_raw);

    if (sig.fpga.video_format_valid) {
        printf("FPGA format=%d (%s) bitdepth=%d\n",
               sig.fpga.video_format_code,
               sig.fpga.video_format,
               sig.fpga.bit_depth);
    }

    if (sig.fpga.frame_rate_valid) {
        printf("FPGA fps code=%s (%s)\n",
               sig.fpga.frame_rate_bits,
               sig.fpga.frame_rate_name);
    }
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
Backend: GVFG | FPGA reported 1280x720 29.97fps | Delivered frame 1280x720 YUY2 8bit | App runtime 29.97fps frames=120
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
[GVFG][XDMA] enumerate: done count=1
[GVFG][XDMA] open_device: done
[GVFG][XDMA] signal: width=1280 height=720
[GVFG][XDMA] configure: effective ... 1280x720 ... frame_bytes=1843200
[SharedScene] preview swapchain created ...
[GVFG][XDMA] start: threads launched
[GVFG][XDMA] publish_frame: id=1 ...
[GVFG][XDMA] wait_frame: deliver id=1 ...
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

## Current XDMA Notes

- The current XDMA path exposes decoded FPGA signal metadata through `sig.fpga`.
- The delivered customer buffer format is reported by `sig.pixel_format` and `sig.bit_depth`.
- `sig.fpga.bit_depth=10` does not by itself mean the delivered buffer is `Y210`.
- Treat the delivered frame as true YUV422 10-bit only when `sig.pixel_format` is `Y210`.
- `sig.fpga.frame_rate_bits=0001` is not in the supported table, so the UI/API reports it as `--`.
