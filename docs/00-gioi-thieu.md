## 00. Giới thiệu dự án

### Mục tiêu bài tập

Dự án `net-driver` triển khai một **kernel module kiểu “companion monitor”** + một **ứng dụng TUI/CLI** để:

- **Giám sát** card mạng USB WiFi **Realtek RTL8188ETV** (USB ID `0bda:0179`)
- **Quét WiFi**, **kết nối/ngắt kết nối**
- **Bắt gói tin** ở tầng network stack, thống kê theo L2/L4, và “deep capture” (lưu snapshot payload)
- **Nhận diện traffic CryptoChat** (port 9090) và hiển thị payload dạng **hex dump** để minh họa dữ liệu đã được mã hóa AES

### Điểm then chốt khi báo cáo (để không bị hỏi “tại sao không viết driver thật?”)

Module `rtl8188_mon.ko` **không thay thế** driver WiFi hệ thống. Driver điều khiển phần cứng thật vẫn là `rtl8xxxu` (thuộc hệ mac80211/cfg80211). Module của bài tập:

- **Không claim USB interface**, không gửi USB transfer → **không xung đột** với `rtl8xxxu`
- Chỉ **quan sát** thiết bị qua USB notifier (plug/unplug)
- Chỉ **quan sát** gói tin qua `dev_add_pack(ETH_P_ALL)` trên interface WiFi đã tồn tại
- Khi cần thao tác WiFi (scan/connect/…) thì **ủy quyền cho userspace tools** (`iw`, `ip`, `nmcli`, …) bằng `call_usermodehelper("/bin/sh -c ...")`

Nói ngắn gọn: đây là một **monitor/assistant** chạy kèm driver thật, không phải NIC driver.

### Các thành phần và giao tiếp

- **Kernel space**
  - `rtl8188_mon.ko`: quản lý state, phát hiện USB, tìm netdev tương ứng, tạo `/dev/rtl8188`, `/proc/rtl8188/*`, packet handler, workqueue cho tác vụ async.
- **Userspace**
  - `rtl8188_cli`: chạy 2 chế độ
    - **TUI (ncurses)**: dashboard nhiều tab, realtime refresh
    - **CLI**: gửi lệnh và in kết quả

Kênh giao tiếp:

- **Char device**: `/dev/rtl8188` (protocol lệnh dạng text)
- **Procfs**: `/proc/rtl8188/{device,stats,status,scan}` (chỉ đọc)

### Những thứ dự án KHÔNG làm (để tránh hiểu nhầm)

- Không có TX/RX path ở mức driver (không can thiệp `mac80211_ops`, không làm DMA/NAPI/IRQ)
- Không có `ioctl`/`ethtool` đặc thù như driver NIC thật
- Không decode AES để ra plaintext (chỉ minh họa ciphertext, và parse header `chat_frame` theo format)

### Nơi xem kiến trúc đầy đủ

Tài liệu nguồn kiến trúc ban đầu nằm ở `ARCHITECTURE.md` (sơ đồ tổng quan + các flow chính).

