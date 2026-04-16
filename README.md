# RTL8188ETV WiFi Companion Monitor Driver

Kernel module + TUI dashboard cho card mạng USB WiFi **Realtek RTL8188ETV** (USB ID `0bda:0179`).

Module chạy **song song** với driver hệ thống `rtl8xxxu` — không thay thế, không xung đột — cung cấp giao diện giám sát, quét WiFi, kết nối mạng và bắt gói tin thông qua character device `/dev/rtl8188`.

## Yêu cầu hệ thống


| Thành phần | Phiên bản                                            |
| ---------- | ---------------------------------------------------- |
| OS         | CentOS / RHEL 10 (aarch64) hoặc tương đương          |
| Kernel     | 6.12.x (đã test trên 6.12.0-214.el10.aarch64)        |
| Hardware   | USB WiFi RTL8188ETV (0bda:0179)                      |
| Packages   | `kernel-devel`, `gcc`, `make`, `ncurses-devel`, `iw` |


```bash
dnf install -y kernel-devel gcc make ncurses-devel iw
```

## Cấu trúc file

```
net-driver/
├── rtl8188_main.c      Module init/exit + helper chung
├── rtl8188_usb.c       USB notifier (detect 0bda:0179)
├── rtl8188_netdev.c    Netdev notifier (track wlan iface)
├── rtl8188_pkt.c       Packet monitor (ETH_P_ALL)
├── rtl8188_cmd.c       Command handlers (scan/connect/stats/capture)
├── rtl8188_proc.c      /proc/rtl8188/*
├── rtl8188_chrdev.c    /dev/rtl8188
├── rtl8188_mon.h       Header dùng chung cho kernel module
├── tui/
│   ├── rtl8188_main_tui.c   main + CLI mode
│   ├── rtl8188_tui.c        TUI core loop + common drawing
│   ├── rtl8188_tabs.c       Tab rendering + parsers
│   └── rtl8188_cli.h        Header dùng chung cho userspace
├── Makefile            Build system
├── README.md           File này
└── ARCHITECTURE.md     Kiến trúc chi tiết
```

## Build & Chạy

```bash
# Build tất cả
make

# Load module (tự động giữ rtl8xxxu chạy song song)
sudo make load

# Mở giao diện TUI
sudo ./rtl8188_cli

# Hoặc dùng CLI truyền thống
sudo ./rtl8188_cli scan
sudo ./rtl8188_cli info
sudo ./rtl8188_cli status
sudo ./rtl8188_cli connect MyWiFi mypassword
sudo ./rtl8188_cli monitor on
sudo ./rtl8188_cli stats
sudo ./rtl8188_cli capture
sudo ./rtl8188_cli disconnect

# Filter gói tin theo port (bắt gói CryptoChat)
sudo ./rtl8188_cli filter chat       # chỉ bắt port 9090
sudo ./rtl8188_cli filter 443        # chỉ bắt HTTPS
sudo ./rtl8188_cli filter 0          # bỏ filter, bắt tất cả
```

## Giao diện TUI

Chạy `./rtl8188_cli` không tham số để mở dashboard toàn màn hình:

```
┌──────────────────────────────────────────────────┐
│       RTL8188ETV WiFi Companion Monitor v1.0     │
├──────────────────────────────────────────────────┤
│ F1 Info  F2 Scan  F3 Status  F4 Monitor          │
│ F5 Capture  F6 Connect                F10 Quit   │
├──────────────────────────────────────────────────┤
│                                                  │
│  (nội dung tab hiện tại)                         │
│                                                  │
├──────────────────────────────────────────────────┤
│ Dev: wlp10s0u4u1 | MAC: 7c:b2:32:87:9a:1a        │
└──────────────────────────────────────────────────┘
```


| Phím  | Tab     | Chức năng                                                                                                                                           |
| ----- | ------- | --------------------------------------------------------------------------------------------------------------------------------------------------- |
| F1    | Info    | Thông tin USB device: vendor, product, endpoints, MAC, tốc độ USB                                                                                   |
| F2    | Scan    | Quét WiFi, hiển thị bảng SSID/BSSID/Channel/Signal. Nhấn `s` để quét. AP đang kết nối highlight xanh                                                |
| F3    | Status  | Trạng thái kết nối hiện tại + địa chỉ IP                                                                                                            |
| F4    | Monitor | Thống kê gói tin real-time (auto-refresh 2s) + biểu đồ L2/L4 protocol + đếm gói CryptoChat AES. `m` bật/tắt monitor, `f` filter chat, `F` bỏ filter |
| F5    | Capture | Deep Packet Capture: IP src/dst, port, hex dump payload (thấy rõ dữ liệu mã hóa AES). `f` filter chat, `F` bỏ filter                                |
| F6    | Connect | Form nhập SSID + Password, hỗ trợ mạng Open và WPA/WPA2                                                                                             |
| F10/q | —       | Thoát                                                                                                                                               |


## Bắt gói tin CryptoChat (AES encrypted)

Ứng dụng chat tại `/root/driver-chat-final` sử dụng:

- **Kernel driver** `crypto_chat` (`/dev/crypto_chat`) cung cấp AES-256-CBC + SHA-256
- **TCP port 9090** cho giao tiếp client-server
- Mỗi message được mã hóa AES-256-CBC trước khi truyền qua mạng

Để bắt và xem gói tin chat mã hóa:

```bash
# 1. Load monitor module
sudo make load

# 2. Mở TUI
sudo ./rtl8188_cli

# 3. Trong TUI:
#    F4 (Monitor) -> nhấn 'm' bật monitoring
#    Nhấn 'f' để filter chỉ port 9090 (CryptoChat)
#    F5 (Capture) -> xem chi tiết gói tin:
#      - IP nguồn/đích + port
#      - Hex dump payload (dữ liệu mã hóa AES)
#      - Gói chat được đánh dấu [CHAT-AES]
```

## Proc filesystem

```bash
cat /proc/rtl8188/device    # Thông tin USB device
cat /proc/rtl8188/stats     # Thống kê gói tin
cat /proc/rtl8188/status    # Trạng thái kết nối
cat /proc/rtl8188/scan      # Kết quả scan gần nhất
```

## Makefile targets

```
make            Build kernel module + TUI
make module     Build kernel module only
make cli        Build TUI/CLI tool only
make load       Load module vào kernel
make unload     Gỡ module
make reload     Gỡ + load lại
make status     Hiện trạng thái hiện tại
make clean      Xóa build artifacts
```

## Lưu ý

- SELinux cần ở chế độ **Permissive** (`sudo setenforce 0`) để `call_usermodehelper` trong kernel module có thể gọi `iw`.
- Module cần quyền **root** để load (`insmod`) và để truy cập `/dev/rtl8188`.
- Driver `rtl8xxxu` của hệ thống phải đang chạy — module companion dựa vào nó để điều khiển hardware.

## Tài liệu báo cáo (chi tiết theo từng chức năng)

Toàn bộ tài liệu giải thích **luồng hoạt động**, **cơ chế**, **giao tiếp kernel↔userspace**, và **cách từng module làm việc** nằm trong thư mục `docs/`:

- Xem mục lục tại `docs/README.md`

