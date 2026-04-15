## 05. Giao tiếp `/dev/rtl8188` (protocol lệnh, sync/async, timeout)

Đây là “xương sống” giao tiếp giữa `rtl8188_cli` và kernel module.

### 5.1 Tạo device node `/dev/rtl8188`

File liên quan: `rtl8188_chrdev.c`

Init `rtl8188_chrdev_init()`:

- `alloc_chrdev_region()` lấy major/minor động
- `cdev_init()` + `cdev_add()` đăng ký `file_operations`
- `class_create(DEVICE_NAME)` tạo class trong sysfs
- `device_create()` để udev tạo node `/dev/rtl8188`

Exit `rtl8188_chrdev_exit()` dọn theo thứ tự ngược.

### 5.2 Model giao tiếp: “write lệnh → read kết quả”

Userspace gửi **chuỗi text** (ASCII) bằng `write()`:

- Ví dụ: `"scan"`, `"info"`, `"monitor on"`, `"filter 9090"`

Kernel xử lý trong `rtl8188_dev_write()`:

- Copy lệnh về buffer kernel (`copy_from_user`)
- Trim `\n`/`\r`
- Set `resp_ready=false`, `resp_len=0` trước khi dispatch
- Dispatch theo chuỗi lệnh
- Reset `*off=0` để lần `read()` luôn đọc từ đầu buffer

Userspace nhận kết quả bằng `read()` (có thể đọc nhiều lần):

- Nếu `resp_ready=true`: trả `resp_buf` theo `*off` (partial read)
- Nếu `resp_ready=false`:
  - `O_NONBLOCK`: trả `-EAGAIN`
  - blocking: `wait_event_interruptible_timeout(..., 60000ms)`
    - timeout → `-ETIMEDOUT`
    - bị signal → errno âm tương ứng

### 5.3 Lệnh sync vs async

#### Lệnh synchronous (trả kết quả ngay)

Các lệnh gọi trực tiếp “generator” (process context) và set response ngay:

- `info` → `generate_info()`
- `status` → `generate_status()`
- `stats` → `generate_stats()`
- `capture` → `generate_capture()`
- `monitor on|off` → đăng ký/hủy packet handler + `set_resp()`
- `filter ...` → set `filter_port`, reset counter, `set_resp()`

Đặc điểm:

- `write()` xong thì `resp_ready` đã true (gần như ngay lập tức)

#### Lệnh asynchronous (workqueue)

Các lệnh cần gọi tool userspace và có thể mất vài giây:

- `scan` → `queue_work(wq, scan_work)`
- `connect <ssid> [pass]` → parse args + sanitize + `queue_work(connect_work)`
- `disconnect` → `queue_work(disconnect_work)`

Đặc điểm:

- `write()` chỉ “đặt việc”
- `read()` sẽ block đến khi workqueue viết response (hoặc timeout)

### 5.4 Tại sao cần sanitize SSID/password

`connect_work_fn()` xây dựng một **script shell** (chuỗi lớn) và chạy qua:

- `call_usermodehelper("/bin/sh -c ...", UMH_WAIT_PROC)`

Để tránh injection khi nhúng SSID/password vào shell, dự án gọi:

- `sanitize(mon->cmd_ssid, ...)`
- `sanitize(mon->cmd_pass, ...)`

sanitize sẽ thay ký tự nguy hiểm (`' " ` $ ; | & \ ( ) < >`) thành `_`.

### 5.5 Quy ước format output (tại sao TUI parse được)

TUI không giao tiếp bằng struct/binary protocol, mà **parse text**.

Một số output có format được “đóng đinh”, ví dụ `generate_capture()`:

- Mỗi packet bắt đầu bằng dòng header **bắt đầu bằng `[`**:
  - `"[N] ..."`
- Dòng địa chỉ bắt đầu bằng `ADDR:`
- Các dòng chi tiết bắt đầu bằng `CF_`, `IV:`, `HMAC1:`, `ENC1:`, `DATA:`…
- Dòng tổng kết: `TOTAL:<n>`

Trong `tui/rtl8188_tabs.c`, hàm `parse_capture_output()` dựa vào các prefix này.

Khi báo cáo, bạn nên nhấn mạnh:

- Đây là giao thức “text protocol” đơn giản, dễ debug (`echo scan > /dev/rtl8188`)
- Đổi format phải đổi cả parser ở userspace

