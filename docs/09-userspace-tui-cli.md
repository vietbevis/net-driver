## 09. Userspace `rtl8188_cli`: TUI (ncurses) và CLI mode

Ứng dụng `rtl8188_cli` là lớp “presentation layer”:

- Chuyển thao tác người dùng thành lệnh text gửi kernel
- Parse text output để vẽ bảng/biểu đồ/chi tiết gói tin

File liên quan:

- `tui/rtl8188_main_tui.c`
- `tui/rtl8188_tui.c`
- `tui/rtl8188_tabs.c`
- Header dùng chung: `tui/rtl8188_cli.h`

---

### 9.1 2 chế độ chạy (entrypoint)

Trong `main()`:

- Nếu `argc >= 2` → **CLI mode**: `cli_main(argc, argv)`
- Nếu `argc == 1` → **TUI mode**: `tui_main()`

Mục đích:

- CLI phù hợp demo nhanh bằng lệnh đơn
- TUI phù hợp demo trực quan realtime

---

### 9.2 Giao tiếp với kernel: `dev_command(cmd, out, out_sz)`

Trong `tui/rtl8188_tui.c`, hàm `dev_command()`:

- `open("/dev/rtl8188", O_RDWR)` rồi `write(cmd)`
- đóng fd
- `open("/dev/rtl8188", O_RDONLY)` rồi `read()` cho đến hết
- đóng fd

Lý do mở 2 lần:

- Tránh vấn đề “file position / partial read”
- Trong kernel `rtl8188_dev_write()` có reset `*off=0`, nên cách mở lại fd giúp chắc chắn đọc từ đầu response.

---

### 9.3 Kiến trúc TUI loop

`tui_main()`:

- init ncurses:
  - `initscr()`, `cbreak()`, `noecho()`, `keypad()`, `timeout(500)`
  - `tui_init_colors()` nếu terminal hỗ trợ màu
- vòng lặp:
  - lấy size terminal, `erase()`
  - vẽ: header, menu (tabs), nội dung tab hiện tại, status bar
  - `refresh()`
  - `getch()` (timeout 500ms) → xử lý input

Các tab chính:

- **F1 Info**: gọi `dev_command("info")` và in từng dòng
- **F2 Scan**:
  - nhấn `s` → `dev_command("scan")` (block vài giây)
  - parse raw output `iw scan` bằng `parse_scan_results()`
  - vẽ bảng AP (SSID/BSSID/channel/signal), hỗ trợ cuộn
- **F3 Status**: `dev_command("status")`
- **F4 Monitor**: `dev_command("stats")`, parse counters, vẽ bar chart L2/L4 + chat count + filter
- **F5 Capture**:
  - `dev_command("capture")`
  - parse thành list + detail panel bằng `parse_capture_output()`
  - UP/DOWN chọn gói, `p` pause danh sách khi đang xem chi tiết
- **F6 Connect**:
  - form nhập SSID/password
  - Enter: gửi `connect ...` và block chờ kết quả

Phím toàn cục:

- F1..F6: đổi tab
- F10 hoặc `q`: thoát (trừ tab Connect có xử lý riêng)
- `m`: toggle monitor on/off (ở tab Monitor)
- `f`: nhập port filter (dialog)
- `F`: xóa filter
- `d`: disconnect (ở tab Status)
- `r`: refresh

---

### 9.4 Parser output (vì sao cần “đúng format”)

#### Parse scan: `parse_scan_results(raw)`

Trong `tui/rtl8188_tabs.c`:

- Duyệt các block bắt đầu bằng `"BSS "`
- Trích:
  - `BSSID` ngay sau `"BSS "`
  - `"freq: "` → đổi ra channel (2.4GHz/5GHz)
  - `"signal: "` → dBm
  - `"SSID: "` → tên mạng
  - `"associated"` → đánh dấu AP đang kết nối

#### Parse capture: `parse_capture_output(buf)`

Dựa vào prefix:

- Dòng bắt đầu bằng `[` + digit → header gói mới
- `ADDR:` → lưu địa chỉ
- Các dòng `CF_`, `IV:`, `HMAC*`, `ENC*`, `DATA:` → lưu vào “detail lines”

Điểm cần nhấn mạnh:

- Đây là “protocol text” đơn giản → đổi format kernel output phải đổi parser tương ứng.

---

### 9.5 Status bar lấy info như nào

`draw_status_bar()` (trong `tui/rtl8188_tui.c`) mỗi lần vẽ sẽ:

- gửi lệnh `info`
- đọc response
- tìm substring `"Interface:"` và `"MAC Address:"` để hiển thị ở đáy màn hình

---

### 9.6 CLI mode hoạt động

`cli_main()`:

- Map argv → command string giống kernel protocol:
  - `scan`, `info`, `status`, `stats`, `capture`, `monitor on|off`, `filter ...`, `connect ...`, `disconnect`
- `dev_command(cmd, buf)`
- in ra stdout

CLI mode phù hợp demo nhanh:

```bash
sudo ./rtl8188_cli info
sudo ./rtl8188_cli scan
sudo ./rtl8188_cli monitor on
sudo ./rtl8188_cli filter chat
sudo ./rtl8188_cli capture
```

