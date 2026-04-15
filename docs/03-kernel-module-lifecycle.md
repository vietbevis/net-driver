## 03. Kernel module lifecycle (init/exit) và state máy

Tài liệu này giải thích “module lên như nào, dọn như nào, và giữ state ra sao”.

### 3.1 State trung tâm: `g_mon` (`struct rtl8188_mon`)

Toàn bộ trạng thái của module nằm trong một struct duy nhất `struct rtl8188_mon` (khai báo trong `rtl8188_mon.h`, instance global `g_mon` trong `rtl8188_main.c`), gồm:

- **USB**: `udev`, `dev_present`, notifier USB
- **Netdev**: `ndev`, `ifname`, notifier netdev
- **Packet monitor**:
  - `ptype` (packet_type đăng ký với network stack)
  - `pkt_registered`, `capture_on`
  - atomic counters: tổng rx, phân loại L2, L4, chat, filter
- **Ring buffer capture**: `ring[]`, `ring_head`, `ring_count`, `ring_lock`
- **Control-plane response**:
  - `resp_buf` (32KB), `resp_len`, `resp_ready`
  - `cmd_lock` (mutex) + `resp_wq` (waitqueue)
- **Workqueue** (async ops): `wq`, `scan_work`, `connect_work`, `disconnect_work`
- **Char dev** `/dev/rtl8188`: `devno`, `cdev`, `cls`, `chrdev`
- **Procfs**: `proc_dir`
- **Uptime**: `load_jiffies`

### 3.2 Thứ tự init: `rtl8188_mon_init()` (từ `rtl8188_main.c`)

Mục tiêu của init là: tạo state, tạo kênh giao tiếp, đăng ký notifier, sẵn sàng nhận lệnh.

Luồng init (tóm tắt đúng theo code):

1. **Cấp phát `g_mon`** bằng `kzalloc`
2. **Cấp phát `resp_buf`** (32KB) để tránh dùng stack
3. **Khởi tạo đồng bộ**:
  - `mutex_init(cmd_lock)`
  - `spin_lock_init(ring_lock)`
  - `init_waitqueue_head(resp_wq)`
4. **Khởi tạo work items**:
  - `INIT_WORK(scan_work, scan_work_fn)`
  - `INIT_WORK(connect_work, connect_work_fn)`
  - `INIT_WORK(disconnect_work, disconnect_work_fn)`
5. **Tạo workqueue 1 thread**: `create_singlethread_workqueue("rtl8188_mon")`
  - Lý do: serialize scan/connect/disconnect, tránh đạp nhau (đều gọi `iw`/`nmcli`)
6. **Tạo char device**: `rtl8188_chrdev_init()` → `/dev/rtl8188`
7. **Tạo procfs**: `rtl8188_proc_init()` → `/proc/rtl8188/`*
8. **USB notifier**: `rtl8188_usb_init()` (register notify + scan USB hiện có)
9. **Netdev notifier**: `rtl8188_netdev_init()`
10. **Rescan netdev nếu device đã có**: nếu `dev_present` thì `scan_existing_netdev()`

Điểm quan trọng khi thuyết trình:

- Module **có thể được insmod sau khi đã cắm USB** nhờ `usb_for_each_dev()` trong `rtl8188_usb_init()`.

### 3.3 Thứ tự exit: `rtl8188_mon_exit()` (từ `rtl8188_main.c`)

Exit dọn theo thứ tự ngược và tránh callback chạy sau khi dọn:

1. Nếu đang monitor: `dev_remove_pack(&ptype)`
2. `cancel_work_sync()` cho scan/connect/disconnect rồi `destroy_workqueue()`
3. Unregister notifier: `rtl8188_netdev_exit()`, `rtl8188_usb_exit()`
4. Xóa procfs: `rtl8188_proc_exit()`
5. Xóa `/dev/rtl8188`: `rtl8188_chrdev_exit()`
6. Nếu còn giữ `udev`: `usb_put_dev()`
7. Xóa file tạm (`/tmp/.rtl8188_`*), free `resp_buf`, free `g_mon`

### 3.4 Đồng bộ hóa (mutex/spinlock/waitqueue) và “ngữ cảnh chạy”

Vì dự án vừa có **process context** (syscall read/write, workqueue) vừa có **softirq context** (packet handler), nên dùng 3 kiểu sync:

- `**cmd_lock` (mutex)**: bảo vệ `resp_buf`, `resp_len`, `resp_ready`
  - Chỉ dùng trong process/workqueue (được sleep)
- `**ring_lock` (spinlock irqsave)**: bảo vệ ring buffer
  - Bắt buộc vì packet handler chạy softirq (không sleep)
- `**resp_wq` (waitqueue)**: cho `read()` block chờ kết quả async

### 3.5 “Máy trạng thái” của response buffer

Response buffer hoạt động theo mô hình “single outstanding command”:

- Khi userspace `write(cmd)`:
  - kernel set `resp_ready=false`, `resp_len=0`
  - dispatch lệnh:
    - lệnh sync: ghi `resp_buf` và set `resp_ready=true` ngay
    - lệnh async: queue work; work xong mới set response
- Khi userspace `read()`:
  - nếu `resp_ready=true`: trả data (hỗ trợ partial read bằng `*off`)
  - nếu `resp_ready=false`:
    - non-blocking: `-EAGAIN`
    - blocking: đợi tối đa 60s rồi `-ETIMEDOUT`

Hệ quả khi báo cáo:

- Module “dạng demo” hỗ trợ **một phản hồi gần nhất**; không có multiplex nhiều client đồng thời.

