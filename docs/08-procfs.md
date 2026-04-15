## 08. Procfs `/proc/rtl8188/*` (read-only “dashboard” cho shell)

Module tạo thư mục `/proc/rtl8188/` và 4 entry con để xem nhanh trạng thái mà không cần chạy TUI.

File liên quan: `rtl8188_proc.c`

### 8.1 Tại sao dùng procfs

- Dễ kiểm tra bằng `cat`, `less`, script shell
- Không phải mở `/dev/rtl8188` theo protocol lệnh
- Thích hợp để demo “kernel exports state”

### 8.2 Cách implement: `proc_create_single()` + `seq_file`

`rtl8188_proc_init()`:

- `proc_mkdir("rtl8188", NULL)` tạo thư mục
- `proc_create_single("device", 0444, dir, proc_device_show)`
- `proc_create_single("stats",  0444, dir, proc_stats_show)`
- `proc_create_single("status", 0444, dir, proc_status_show)`
- `proc_create_single("scan",   0444, dir, proc_scan_show)`

Điểm mạnh của `proc_create_single()`:

- Kernel tự cung cấp glue code của `seq_file` (read/seek)
- Hàm `*_show()` chỉ cần “in” vào `seq_file`

### 8.3 Ý nghĩa từng entry

#### `/proc/rtl8188/device`

`proc_device_show()`:

- Nếu `dev_present && udev`:
  - vendor/product, usb speed
  - interface name (`ifname`)
  - MAC (nếu `ndev` có)
- Uptime module

#### `/proc/rtl8188/stats`

`proc_stats_show()`:

- In counters (atomic) của traffic monitor: rx pkts/bytes, L2, L4, chat
- Nếu có `ndev`: in thêm driver stats qua `dev_get_stats()`
- Uptime module

#### `/proc/rtl8188/status`

`proc_status_show()`:

- Best-effort gọi `iw dev <ifname> link` qua `run_cmd()`
- Đọc file tạm `/tmp/.rtl8188_status`
- In nội dung ra proc

Lưu ý:

- Đây là “pull model” (mỗi lần cat là chạy `iw`), nên phụ thuộc SELinux/permissions.

#### `/proc/rtl8188/scan`

`proc_scan_show()`:

- **Không chạy scan mới**
- Chỉ in `resp_buf` hiện tại (nếu trước đó vừa scan qua `/dev/rtl8188`)
- Nếu chưa có, in hướng dẫn:
  - `echo scan > /dev/rtl8188`

### 8.4 Ví dụ demo nhanh

```bash
cat /proc/rtl8188/device
cat /proc/rtl8188/stats
cat /proc/rtl8188/status
cat /proc/rtl8188/scan
```

