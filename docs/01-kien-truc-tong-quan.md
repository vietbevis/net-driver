## 01. Kiến trúc tổng quan (big picture)

### Sơ đồ thành phần

Hệ thống có 2 phần: **kernel module** và **userspace TUI/CLI**, giao tiếp qua `/dev/rtl8188`.

```
┌────────────────────────────────────────────────────────────────────┐
│                            USERSPACE                                │
│  rtl8188_cli                                                        │
│  - CLI: ./rtl8188_cli scan|info|status|...                          │
│  - TUI: dashboard nhiều tab (ncurses)                               │
│                                                                     │
│             write()/read() lệnh text qua /dev/rtl8188               │
└────────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌────────────────────────────────────────────────────────────────────┐
│                           KERNEL SPACE                              │
│  rtl8188_mon.ko                                                     │
│  - USB notifier: phát hiện 0bda:0179                                │
│  - Netdev notifier: tìm interface WiFi tương ứng                    │
│  - Packet handler: dev_add_pack(ETH_P_ALL)                          │
│  - Workqueue: scan/connect/disconnect (async)                       │
│  - /proc/rtl8188/*                                                  │
│                                                                     │
│  Thao tác WiFi “delegate” cho userspace tools qua call_usermodehelper│
└────────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌────────────────────────────────────────────────────────────────────┐
│        Driver thật (system) + subsystem WiFi của kernel             │
│  rtl8xxxu + mac80211/cfg80211                                       │
│  - claim USB, quản lý firmware/PHY                                  │
│  - tạo interface wlan*                                              │
│  - xử lý 802.11 TX/RX                                               │
└────────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌────────────────────────────────────────────────────────────────────┐
│                         HARDWARE USB WiFi                           │
│                      RTL8188ETV (0bda:0179)                          │
└────────────────────────────────────────────────────────────────────┘
```

### Triết lý thiết kế “companion”

Lý do chọn “companion module” thay vì viết driver USB WiFi mới:

- Tránh phạm vi quá lớn (driver thật cần firmware, PHY/RF, mac80211 ops…)
- Không đụng phần nhạy cảm: claim device, bulk transfers
- Tập trung vào luồng dữ liệu & giám sát (monitoring/capture) và kênh giao tiếp kernel↔userspace

### Mapping USB device ↔ network interface

Bài toán: `rtl8188_mon` chỉ thấy **USB device** (VID/PID), nhưng packet monitor cần **net_device** (interface wlan*).

Cách làm:

- Khi thấy USB device, module lưu `usb_device *udev`
- Khi netdev event xảy ra (REGISTER/UP/...), module kiểm tra dev đó có phải do USB device này tạo ra không bằng cách:
  - Duyệt `ndev->dev.parent` lên `parent->parent->...`
  - Nếu gặp `&udev->dev` → đó là interface của mình

Kết quả: lưu `g_mon->ndev` và `g_mon->ifname`.

### Hai mặt phẳng chức năng: Control-plane và Data-plane

- **Control-plane (điều khiển)**: scan/connect/status/info/stats/filter/monitor…
  - Userspace gửi lệnh text → kernel dispatch
  - Một số lệnh sync (trả ngay), một số lệnh async (workqueue)
- **Data-plane (dữ liệu/gói tin)**: packet monitoring/capture
  - Khi bật monitor: kernel đăng ký packet handler trên `g_mon->ndev`
  - Mỗi packet → deep inspect → counters + ring buffer snapshot

### Các file chính tương ứng “từng khối”

- **Core + lifecycle**: `rtl8188_main.c`, `rtl8188_mon.h`
- **USB detect**: `rtl8188_usb.c`
- **Netdev tracking**: `rtl8188_netdev.c`
- **Char device `/dev/rtl8188`**: `rtl8188_chrdev.c`
- **Command handlers + workqueue**: `rtl8188_cmd.c`
- **Packet monitor/capture**: `rtl8188_pkt.c`
- **Procfs**: `rtl8188_proc.c`
- **TUI/CLI**: `tui/rtl8188_main_tui.c`, `tui/rtl8188_tui.c`, `tui/rtl8188_tabs.c`

