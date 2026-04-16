## 10. Troubleshooting (lỗi thường gặp khi demo/báo cáo)

### 10.1 Không thấy `/dev/rtl8188`

Nguyên nhân:

- Module chưa load
- `insmod` lỗi do kernel headers không khớp
- Udev chưa tạo node (hiếm)

Checklist:

- `sudo make load`
- `dmesg | tail` (xem log `[rtl8188_mon]`)
- `ls -la /dev/rtl8188`
- `lsmod | grep rtl8188`

### 10.2 Module load rồi nhưng “không tìm thấy wireless interface”

Thông báo thường gặp trong scan/connect/status:

- `LỖI: Không tìm thấy wireless interface.`

Nguyên nhân:

- Driver hệ thống `rtl8xxxu` chưa chạy
- USB WiFi chưa được nhận dạng hoặc không đúng VID/PID
- Race: module load trước khi netdev REGISTER (thường tự hết sau vài giây)

Checklist:

- `lsusb | grep 0bda:0179`
- `lsmod | grep rtl8xxxu`
- `ip link | grep -E 'wl|wlan'`
- Thử `sudo make reload` sau khi đã cắm USB

### 10.3 Scan/connect timeout hoặc trả lỗi `ret=-13`

Triệu chứng:

- `scan` không trả dữ liệu, hoặc `read()` timeout
- Trong output có `ret=-13` hoặc “permission denied”

Nguyên nhân:

- SELinux **Enforcing** chặn `call_usermodehelper()` (EACCES)
- `iw`/`ip` không có hoặc path khác

Fix:

- `sudo setenforce 0` (runtime)
- Đảm bảo `iw` tồn tại ở `/usr/sbin/iw` (dự án hardcode path này)
- Cài deps theo `install.sh`

### 10.4 `monitor on` nhưng không thấy gói trong Capture

Nguyên nhân:

- Không có traffic (idle)
- Filter port đang bật và không có traffic match
- Bật `monitor on` nhưng chưa có `ndev`

Checklist:

- Trong TUI/CLI đảm bảo đã chạy `monitor on`
- Xóa filter: `filter 0`
- Tạo traffic: ping, curl, hoặc chạy CryptoChat để thấy port 9090

### 10.5 Capture thấy “rác” (không đọc được)

Đây là **đúng mong đợi** với CryptoChat:

- payload là ciphertext AES-256-CBC
- module chỉ dump hex + parse header `chat_frame` (IV/HMAC/ENC preview)

Khi báo cáo, bạn có thể nói:

- “Chính vì mã hóa nên packet capture không đọc plaintext”
- “Mình chứng minh bằng việc parse IV/HMAC, còn plaintext chỉ endpoint có key mới giải được”

### 10.6 `connect` không lên IP

Nguyên nhân:

- NetworkManager không chạy hoặc cấu hình khác distro
- DHCP client không chạy/không cấp phát

Gợi ý:

- Xem output chi tiết từ `connect` (script ghi vào `/tmp/.rtl8188_connect`)
- Thử `status` để xem `iw link` có Connected chưa
- Nếu Connected mà chưa có IP: kiểm tra DHCP/NetworkManager của hệ thống

