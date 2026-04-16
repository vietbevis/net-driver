## 07. CryptoChat: nhận diện và parse `chat_frame` trong capture

Mục tiêu của phần này là giải thích rõ:

- Vì sao trong Capture tab nhìn thấy payload “trông như rác” (ciphertext)
- Module nhận diện “gói chat” bằng cách nào
- Module parse được những trường nào trong `chat_frame` và hiển thị ra sao

> Lưu ý: dự án **không giải mã AES** trong module này. Nó chỉ minh họa dữ liệu đã được mã hóa và trích một số field header để chứng minh “đây đúng là frame CryptoChat”.

---

### 7.1 Mốc nhận diện đơn giản: port 9090

Trong `rtl8188_pkt_recv()`:

- Nếu `sport==9090` hoặc `dport==9090`:
  - set `is_chat=1`
  - tăng `chat_cnt`

Đây là heuristic nhanh để demo.

---

### 7.2 Nhận diện theo nội dung: parse header `chat_frame`

Header/offset của `chat_frame` được định nghĩa trong `rtl8188_mon.h`:

- **CF_HDR_SIZE = 52**
  - 1 byte `ver`
  - 1 byte `type`
  - 2 byte `plen` (little-endian)
  - 16 byte `IV` (AES-CBC)
  - 32 byte `HMAC` (SHA-256)
  - sau đó là ciphertext payload

Offset:

- `CF_OFF_VER = 0`
- `CF_OFF_TYPE = 1`
- `CF_OFF_PLEN = 2` (u16 little-endian)
- `CF_OFF_IV = 4` (16 bytes)
- `CF_OFF_HMAC = 20` (32 bytes)
- `CF_OFF_PAYLOAD = 52`

#### Điều kiện “có vẻ hợp lệ” mà module kiểm tra

Hàm `try_parse_chat_frame(e)` trong `rtl8188_pkt.c` chỉ chạy sau khi snapshot payload, và yêu cầu:

- `payload_len >= 52`
- `ver == 1`
- `type` nằm trong khoảng các `MSG_TYPE_*` (AUTH…REG_FAIL)

Nếu pass:

- `chat_parsed = 1`
- trích:
  - `chat_ver`
  - `chat_type` và map sang tên (AUTH/CHAT/…)
  - `chat_plen` (đọc little-endian)
  - `chat_iv[16]`
  - `chat_hmac[32]`
  - `chat_enc[<=32]` (preview ciphertext)

Nếu packet chưa bị đánh dấu chat theo port:

- module vẫn set `is_chat=1` và `chat_cnt++`

---

### 7.3 Vì sao thấy ciphertext (hex dump) chứ không thấy plaintext

Luồng CryptoChat (theo mô tả README/ARCHITECTURE):

1. Ứng dụng chat mã hóa message bằng AES-256-CBC
2. Gói TCP mang payload là `chat_frame`:
   - header + ciphertext
3. Monitor module bắt gói ở network stack:
   - nó chỉ thấy bytes đã mã hóa

Do đó Capture tab hiển thị:

- `ENC1/ENC2`: preview ciphertext
- hoặc `DATA:`: hex dump payload snapshot

Điểm quan trọng khi thuyết trình:

- Dự án này chứng minh “mã hóa làm payload không đọc được” ở mức packet capture
- Việc giải mã đúng chỉ xảy ra ở endpoint có key (driver/`/dev/crypto_chat` bên dự án CryptoChat), không nên làm trong monitor.

---

### 7.4 TUI hiển thị `chat_frame` như nào

`generate_capture()` in thêm các dòng khi `chat_parsed=1`:

- `CF_VER:...`
- `CF_TYPE:0x..(NAME)`
- `CF_PLEN:...`
- `IV:...` (màu nổi bật)
- `HMAC1:` + `HMAC2:`
- `ENC1:` + `ENC2:` (màu đỏ, nhấn mạnh ciphertext)

Trong `tui/rtl8188_tabs.c`:

- `parse_capture_output()` gom các dòng bắt đầu bằng `CF_`, `IV:`, `HMAC`, `ENC`, `DATA`
- `draw_tab_capture()`:
  - danh sách gói: highlight nếu có `[CHAT-AES]`
  - panel chi tiết: tô màu riêng cho IV/HMAC/ENC

