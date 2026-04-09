# Kiến trúc hệ thống - RTL8188ETV WiFi Companion Monitor

## Tổng quan

Hệ thống gồm 2 thành phần: **Kernel Module** (`rtl8188_mon.ko`) và **Userspace TUI** (`rtl8188_cli`), giao tiếp qua character device `/dev/rtl8188`.

```
┌─────────────────────────────────────────────────────────────────┐
│                        USERSPACE                                │
│                                                                 │
│  ┌───────────────────────────────────────────────────────┐      │
│  │              rtl8188_cli (TUI / CLI)                  │      │
│  │                                                       │      │
│  │  ┌──────┐ ┌──────┐ ┌────────┐ ┌─────────┐ ┌───────┐ │      │
│  │  │ Info │ │ Scan │ │ Status │ │ Monitor │ │Connect│ │      │
│  │  └──┬───┘ └──┬───┘ └───┬────┘ └────┬────┘ └───┬───┘ │      │
│  │     │        │         │            │          │     │      │
│  │     └────────┴────┬────┴────────────┴──────────┘     │      │
│  │                   │                                   │      │
│  │          write()/read() qua /dev/rtl8188              │      │
│  └───────────────────┼───────────────────────────────────┘      │
│                      │                                          │
├──────────────────────┼──────────────────────────────────────────┤
│                      │           KERNEL SPACE                   │
│                      ▼                                          │
│  ┌───────────────────────────────────────────────────────┐      │
│  │              rtl8188_mon.ko (Kernel Module)           │      │
│  │                                                       │      │
│  │  ┌─────────────┐  ┌──────────────┐  ┌─────────────┐ │      │
│  │  │  Char Dev   │  │  Proc FS     │  │  Workqueue   │ │      │
│  │  │ /dev/rtl8188│  │ /proc/rtl8188│  │  (async ops) │ │      │
│  │  └──────┬──────┘  └──────────────┘  └──────┬──────┘ │      │
│  │         │                                   │        │      │
│  │  ┌──────┴──────────────────────────────────┐│        │      │
│  │  │          Command Dispatcher             ││        │      │
│  │  │  scan | connect | disconnect | status   ││        │      │
│  │  │  info | stats | monitor | capture       ││        │      │
│  │  └──────┬──────────────────────────────────┘│        │      │
│  │         │                                   │        │      │
│  │  ┌──────┴──────┐  ┌────────────┐  ┌────────┴──────┐ │      │
│  │  │ USB Notifier│  │ Netdev     │  │  Packet       │ │      │
│  │  │ (detect     │  │ Notifier   │  │  Handler      │ │      │
│  │  │  0bda:0179) │  │ (track     │  │  (dev_add_pack│ │      │
│  │  │             │  │  wlan iface│  │   ETH_P_ALL)  │ │      │
│  │  └──────┬──────┘  └──────┬─────┘  └────────┬──────┘ │      │
│  │         │                │                  │        │      │
│  │  ┌──────┴────────────────┴──────────────────┴──────┐ │      │
│  │  │         call_usermodehelper (/usr/sbin/iw)      │ │      │
│  │  └──────┬──────────────────────────────────────────┘ │      │
│  └─────────┼─────────────────────────────────────────────┘      │
│            │                                                    │
│  ┌─────────┴─────────────────────────────────────────────┐      │
│  │              rtl8xxxu (System WiFi Driver)            │      │
│  │              mac80211 / cfg80211 subsystem             │      │
│  └─────────┬─────────────────────────────────────────────┘      │
│            │                                                    │
├────────────┼────────────────────────────────────────────────────┤
│            │                  HARDWARE                          │
│  ┌─────────┴─────────────────────────────────────────────┐      │
│  │     RTL8188ETV USB WiFi Adapter (0bda:0179)           │      │
│  │     USB 2.0 High-Speed | 802.11b/g/n | 2.4GHz        │      │
│  └───────────────────────────────────────────────────────┘      │
└─────────────────────────────────────────────────────────────────┘
```

## Thành phần chi tiết

### 1. Kernel Module (multi-file: `rtl8188_main.c` + `rtl8188_*.c`)

#### 1.1 USB Device Detection

```
usb_register_notify() ──► Callback khi USB device plug/unplug
                          │
                          ├── USB_DEVICE_ADD:  Lưu usb_device pointer
                          │                    Quét netdev hiện tại
                          │
                          └── USB_DEVICE_REMOVE: Giải phóng reference
                                                 Dừng packet monitor

usb_for_each_dev()   ──► Quét USB bus khi module load
                         Tìm device có VID=0x0bda, PID=0x0179
```

**Quan trọng**: Module KHÔNG dùng `usb_register()` (sẽ xung đột với `rtl8xxxu`). Chỉ dùng `usb_register_notify()` để theo dõi passively.

#### 1.2 Network Interface Tracking

```
register_netdevice_notifier() ──► Callback khi network interface thay đổi
                                  │
                                  ├── NETDEV_REGISTER: Lưu net_device pointer
                                  ├── NETDEV_UP:       Log interface up
                                  ├── NETDEV_DOWN:     Log interface down
                                  ├── NETDEV_UNREGISTER: Xóa pointer, dừng monitor
                                  └── NETDEV_CHANGENAME: Cập nhật tên interface

is_our_iface()  ──► Duyệt device tree từ net_device lên parent
                    Kiểm tra có match usb_device đã lưu không
```

#### 1.3 Character Device (`/dev/rtl8188`)

```
write() ──► Nhận lệnh text từ userspace
            │
            ├── "scan"              ──► queue_work(scan_work)      [async]
            ├── "connect SSID PW"   ──► queue_work(connect_work)   [async]
            ├── "disconnect"        ──► queue_work(disconnect_work) [async]
            ├── "status"            ──► generate_status()           [sync]
            ├── "info"              ──► generate_info()             [sync]
            ├── "stats"             ──► generate_stats()            [sync]
            ├── "monitor on|off"    ──► dev_add/remove_pack()       [sync]
            ├── "capture"           ──► generate_capture()          [sync]
            ├── "filter <port>"     ──► Set filter_port             [sync]
            ├── "filter chat"       ──► Set filter_port = 9090      [sync]
            └── "filter 0"          ──► Remove filter               [sync]

read()  ──► Trả về kết quả lệnh cuối cùng
            │
            ├── Nếu resp_ready: trả data ngay
            ├── Nếu chưa ready + blocking: wait_event_interruptible_timeout (20s)
            └── Nếu chưa ready + O_NONBLOCK: return -EAGAIN
```

#### 1.4 Packet Monitoring

```
dev_add_pack(ETH_P_ALL) ──► Đăng ký packet handler cho interface WiFi
                            │
                            ▼
                     rtl8188_pkt_recv()  [Deep Packet Inspection]
                            │
                            ├── Đếm L2: rx_pkts, rx_bytes
                            ├── Phân loại L2: ARP / IPv4 / IPv6 / Other
                            │
                            ├── IPv4 Deep Inspection:
                            │   ├── Parse IP header ──► src_ip, dst_ip, protocol
                            │   ├── TCP ──► src_port, dst_port, payload snapshot
                            │   ├── UDP ──► src_port, dst_port, payload snapshot
                            │   └── ICMP ──► count only
                            │
                            ├── CryptoChat Detection:
                            │   └── port == 9090? ──► đánh dấu is_chat, đếm chat_cnt
                            │
                            ├── Port Filter:
                            │   └── filter_port != 0? ──► bỏ qua gói không match
                            │
                            └── capture_on?
                                ├── Yes ──► Lưu vào ring buffer (128 entries)
                                │          {timestamp, src/dst IP, ports, protocol,
                                │           payload hex dump (64 bytes), is_chat flag}
                                └── No  ──► Chỉ đếm thống kê
```

**Ring buffer**: Circular buffer 128 entries, spinlock bảo vệ, không cấp phát bộ nhớ trong interrupt context.

**Payload Snapshot**: Mỗi gói tin lưu tối đa 64 bytes đầu của TCP/UDP payload. Với gói CryptoChat, đây là dữ liệu đã được mã hóa AES-256-CBC — hiển thị dưới dạng hex dump để minh họa quá trình mã hóa.

#### 1.5 Async Operations (Workqueue)

```
scan_work_fn()
    │
    ├── ip link set <iface> up
    ├── iw dev <iface> scan > /tmp/.rtl8188_scan    (hoặc scan dump nếu busy)
    ├── kernel_read() đọc kết quả từ file
    └── Cập nhật resp_buf, đánh thức wait queue

connect_work_fn()
    │
    ├── Có password?
    │   ├── Yes ──► Tạo wpa_supplicant.conf (heredoc)
    │   │          wpa_supplicant -B
    │   │          dhclient
    │   └── No  ──► iw dev <iface> connect <ssid>
    │
    └── iw dev <iface> link ──► Cập nhật kết quả

disconnect_work_fn()
    │
    ├── killall wpa_supplicant
    └── iw dev <iface> disconnect
```

Tất cả async operations chạy trên `create_singlethread_workqueue("rtl8188_mon")`, đảm bảo serialize.

#### 1.6 Proc Filesystem

```
/proc/rtl8188/
    ├── device   (0444)  ──► proc_device_show()  ──► USB info, MAC
    ├── stats    (0444)  ──► proc_stats_show()   ──► Packet counters
    ├── status   (0444)  ──► proc_status_show()  ──► iw link output
    └── scan     (0444)  ──► proc_scan_show()    ──► Last scan results
```

Dùng `proc_create_single()` + `seq_file` interface.

### 2. TUI Dashboard (multi-file: `tui/rtl8188_*.c`)

```
main()
  │
  ├── argc >= 2 ──► cli_main()     CLI truyền thống
  │                 write()/read() qua /dev/rtl8188
  │                 In kết quả ra stdout
  │
  └── argc == 1 ──► tui_main()     Giao diện ncurses
                    │
                    ├── initscr(), cbreak(), noecho()
                    ├── init_colors() (16 color pairs)
                    │
                    └── Main loop (timeout 500ms):
                        │
                        ├── draw_header()     ──► Title bar (blue bg)
                        ├── draw_menu()       ──► Tab bar (cyan bg, selected=blue)
                        ├── draw_tab_*()      ──► Nội dung tab hiện tại
                        ├── draw_status_bar() ──► Bottom bar (device info)
                        │
                        └── getch() ──► Xử lý input
                            ├── F1-F6: Chuyển tab
                            ├── F10/q: Thoát
                            ├── s: Scan (tab Scan)
                            ├── m: Toggle monitor (tab Monitor)
                            ├── d: Disconnect (tab Status)
                            ├── r: Force refresh
                            └── UP/DOWN: Scroll
```

#### Tab Scan — Parse iw output

```
Raw iw scan output ──► parse_scan_results()
                       │
                       ├── Tìm "BSS xx:xx:xx:xx:xx:xx"
                       ├── Trích "freq: XXXX" → freq_to_channel()
                       ├── Trích "signal: -XX dBm"
                       ├── Trích "SSID: ..."
                       ├── Check "-- associated"
                       │
                       └── Mảng ap_entry[64]
                           {bssid, ssid, channel, signal, associated}

Hiển thị:
  - Signal >= -50 dBm ──► Xanh lá (tốt)
  - Signal >= -70 dBm ──► Vàng (trung bình)
  - Signal <  -70 dBm ──► Đỏ (yếu)
  - AP đang kết nối   ──► Xanh lá bold + "<<< connected"
```

#### Tab Monitor — Bar chart

```
Lấy stats từ module ──► Parse L2 + L4 counts
                        │
                        ├── L2 Protocol Distribution:
                        │   ARP  ███████░░░░░░░  (magenta)
                        │   IPv4 ████████████░░  (green)
                        │   IPv6 ██░░░░░░░░░░░░  (cyan)
                        │   Oth  █░░░░░░░░░░░░░  (yellow)
                        │
                        ├── L4 Protocol Distribution:
                        │   TCP  █████████████░  (blue)
                        │   UDP  ████░░░░░░░░░░  (magenta)
                        │   ICMP █░░░░░░░░░░░░░  (white)
                        │
                        └── ** CryptoChat AES Packets: N **  (red, bold)
```

Auto-refresh mỗi 2 giây qua ncurses `timeout(500)` + time check.

#### Tab Capture — Deep Packet Inspection

```
Mỗi gói tin hiển thị:
  [0] 3s ago | IPv4/TCP | 1234 bytes [CHAT-AES]
      192.168.1.100:54321 -> 192.168.1.1:9090
      DATA[64]: 7a 3b c1 ff 02 8e a4 ... (encrypted AES payload)

  [1] 5s ago | IPv4/UDP | 78 bytes
      10.0.0.1:53 -> 10.0.0.100:12345
      DATA[42]: 01 00 00 01 00 ... (DNS query)

Gói CryptoChat được đánh dấu [CHAT-AES] (đỏ bold)
Hex dump thể hiện rõ dữ liệu đã được mã hóa AES-256-CBC
```

## Luồng dữ liệu

### Scan WiFi

```
User nhấn 's' (TUI)
    │
    ▼
TUI write("scan") ──► /dev/rtl8188
    │
    ▼
Kernel: rtl8188_dev_write() ──► queue_work(scan_work)
    │                           return ngay
    ▼
TUI read() ──► block chờ resp_ready (tối đa 20s)
    │
    ▼
Kernel workqueue: scan_work_fn()
    ├── call_usermodehelper("iw dev wlanX scan")
    ├── kernel_read("/tmp/.rtl8188_scan")
    ├── resp_buf = kết quả
    ├── resp_ready = true
    └── wake_up_interruptible()
    │
    ▼
TUI read() unblock ──► nhận raw iw output
    │
    ▼
TUI parse_scan_results() ──► bảng AP đẹp
```

### Packet Monitoring

```
Packet vào WiFi interface (từ rtl8xxxu)
    │
    ▼
Linux networking stack
    │
    ├──► Normal processing (IP stack, socket...)
    │
    └──► Clone skb ──► rtl8188_pkt_recv()    (dev_add_pack hook)
                       │
                       ├── atomic_inc(rx_pkts)
                       ├── L2 classify: ARP/IPv4/IPv6/Other
                       ├── L4 classify: TCP/UDP/ICMP
                       ├── Deep inspect: parse IP header, TCP/UDP header
                       │   ├── Extract src_ip:port -> dst_ip:port
                       │   ├── Snapshot 64 bytes of L4 payload
                       │   └── Detect CryptoChat (port 9090)
                       ├── Port filter check (skip if no match)
                       ├── Ring buffer: lưu full packet metadata + payload hex
                       └── kfree_skb(clone)
```

### Bắt gói CryptoChat (AES-256-CBC)

```
CryptoChat App                      RTL8188 Monitor
──────────────                      ───────────────
gui_client.c                         rtl8188_pkt_recv()
    │                                     │
    ├── Nhập message "Hello"              │
    ├── IOCTL_AES_ENCRYPT                 │
    │   (/dev/crypto_chat)                │
    ├── AES-256-CBC encrypt               │
    ├── Build chat_frame:                 │
    │   {version, type, IV, HMAC,         │
    │    encrypted_payload}               │
    │                                     │
    ├── TCP send() port 9090 ────────────►├── Nhận gói TCP
    │                                     ├── port == 9090? ──► [CHAT-AES]
    │                                     ├── Lưu IP src/dst, ports
    │                                     ├── Snapshot 64 bytes payload
    │                                     │   (= encrypted AES ciphertext)
    │                                     └── Hiển thị hex dump trong TUI
    │
    ▼
server.c (port 9090)
    ├── Nhận chat_frame
    ├── Verify HMAC
    ├── IOCTL_AES_DECRYPT
    └── Forward encrypted_frame to recipient
```

## Đồng bộ hóa

| Resource | Cơ chế | Ngữ cảnh |
|---|---|---|
| `resp_buf` (response buffer) | `cmd_lock` (mutex) | Process context (write/read/workqueue) |
| `ring[]` (capture buffer) | `ring_lock` (spinlock + irqsave) | Softirq (packet handler) + process |
| Atomic counters (rx_pkts, ...) | `atomic64_t` / `atomic_t` | Lock-free, mọi context |
| `resp_ready` flag | `wait_queue_head_t` | Wakeup từ workqueue, sleep trong read() |

## Quan hệ với driver hệ thống

```
rtl8188_mon.ko (companion)          rtl8xxxu (system driver)
─────────────────────               ────────────────────────
Không claim USB device              Claim USB device (usb_register)
Không gửi USB transfers             Gửi/nhận USB bulk transfers
Đọc thông tin qua notifier          Điều khiển hardware trực tiếp
Gọi iw/wpa_supplicant               Implement mac80211_ops
Bắt packet qua dev_add_pack         Xử lý TX/RX 802.11 frames
                                    Quản lý firmware, PHY, RF
```

Hai module hoạt động hoàn toàn độc lập — `rtl8188_mon` chỉ **quan sát** và **delegate** thao tác WiFi cho hệ thống thông qua userspace tools (`iw`, `wpa_supplicant`).
