## 06. Packet monitoring & capture (dev_add_pack, deep inspect, ring buffer)

Đây là phần “data-plane”: module bắt và phân tích gói tin đi qua interface WiFi.

File liên quan: `rtl8188_pkt.c` + phần bật/tắt monitor trong `rtl8188_chrdev.c`.

---

### 6.1 Bật/tắt monitor thực tế làm gì

Userspace gửi:

- `monitor on`
- `monitor off`

Trong `rtl8188_dev_write()`:

- `monitor on`:
  - nếu chưa `pkt_registered` và đã có `mon->ndev`:
    - cấu hình `mon->ptype`:
      - `ptype.type = htons(ETH_P_ALL)`
      - `ptype.func = rtl8188_pkt_recv`
      - `ptype.dev  = mon->ndev`
    - `dev_add_pack(&ptype)`
    - `pkt_registered = true`
  - `capture_on = true`
- `monitor off`:
  - `capture_on = false`
  - nếu `pkt_registered`:
    - `dev_remove_pack(&ptype)`
    - `pkt_registered = false`

Lưu ý quan trọng:

- `dev_add_pack()` đăng ký handler ở network stack, **không đụng driver**.
- `capture_on` quyết định có deep inspect + lưu ring buffer hay chỉ đếm.

---

### 6.2 Tại sao `dev_add_pack(ETH_P_ALL)` nhận được gói tin

Linux networking stack có cơ chế “packet taps” bằng `packet_type`:

- Mỗi packet đi qua stack, kernel sẽ gọi các handler đăng ký phù hợp.
- Khi type là `ETH_P_ALL`, handler nhận tất cả EtherType.

Trong dự án:

- handler chỉ xử lý packet của đúng interface:
  - `if (dev != g_mon->ndev) goto out;`

---

### 6.3 Ngữ cảnh chạy và ràng buộc softirq

`rtl8188_pkt_recv()` chạy trong softirq context (đường xử lý mạng), nên:

- **Không được sleep**
- Không được làm thao tác blocking
- Tránh cấp phát lớn với `GFP_KERNEL`

Dự án đảm bảo:

- Không gọi `run_cmd()` trong packet handler
- Ring buffer là mảng fixed-size nằm trong `g_mon` (không allocate mỗi packet)
- Snapshot payload dùng `skb_copy_bits()` (an toàn với skb nonlinear)

---

### 6.4 Các lớp phân tích (L2 → IPv4 → L4)

#### (1) Counters tổng

Ngay khi vào handler (dù có capture hay không):

- `rx_pkts++`, `rx_bytes += skb->len`
- phân loại L2 theo `skb->protocol`:
  - ARP/IPv4/IPv6/Other

#### (2) Deep inspect chỉ khi `capture_on`

Nếu `capture_on=false` → thoát sớm, chỉ còn counters.

Nếu `capture_on=true` và là IPv4:

- Lấy `iph = ip_hdr(skb)`
- `l4_proto = iph->protocol`
- Nếu TCP:
  - parse `tcphdr`, lấy `sport/dport`, payload pointer + payload_len
  - `tcp_cnt++`
- Nếu UDP:
  - parse `udphdr`, lấy `sport/dport`, payload pointer + payload_len
  - `udp_cnt++`
- ICMP: `icmp_cnt++` (không snapshot payload)

---

### 6.5 Port filter

`g_mon->filter_port` (0 = không lọc).

Cơ chế:

- Handler vẫn có thể đếm `filter_cnt` nếu packet match port, để tab Monitor hiển thị “Matched pkts”
- Nhưng **ring buffer chỉ lưu** packet nếu:
  - `filter_port == 0` hoặc `sport == filter_port` hoặc `dport == filter_port`

Hệ quả khi demo:

- Nếu bạn bật filter, Capture tab sẽ chỉ thấy traffic port đó.

---

### 6.6 CryptoChat detection (2 lớp)

Dự án đánh dấu `is_chat` theo:

1. **Theo port mặc định**: nếu `sport==9090` hoặc `dport==9090`:
   - `is_chat=1`, `chat_cnt++`
2. **Theo nội dung frame**: `try_parse_chat_frame(e)`:
   - Nếu payload snapshot đủ lớn và giống format `chat_frame`:
     - set `chat_parsed=1`, trích IV/HMAC/ciphertext preview
     - nếu chưa `is_chat` thì cũng đánh dấu và `chat_cnt++`

Điểm hay để nói khi báo cáo:

- Port-based là heuristic đơn giản
- Content-based giúp nhận diện dù chat chạy trên port khác

---

### 6.7 Ring buffer capture (circular buffer)

Ring buffer nằm trong `g_mon`:

- `ring[CAPTURE_RING_SIZE]` (128 entry)
- `ring_head`: vị trí ghi tiếp theo
- `ring_count`: số entry hợp lệ

Ghi ring:

- giữ `spin_lock_irqsave(ring_lock)`
- ghi vào `ring[ring_head]`
- `ring_head = (ring_head+1) % SIZE`
- nếu chưa đầy thì `ring_count++`, nếu đầy thì overwrite entry cũ nhất

Thiết kế này đảm bảo:

- O(1) cho mỗi packet
- Không tăng memory theo thời gian

---

### 6.8 Snapshot payload: vì sao dùng `skb_copy_bits()`

`skb` có thể là:

- linear data
- hoặc non-linear (paged fragments)

Nếu đọc payload bằng con trỏ trực tiếp có thể sai/unsafe. Dự án:

- tính offset của payload trong skb
- `skb_copy_bits(skb, off, e->payload, snap)`

Kết quả:

- `e->payload_len = min(payload_len, PAYLOAD_SNAP_SIZE)`
- lưu tối đa ~1280 byte (theo `PAYLOAD_SNAP_SIZE`)

---

### 6.9 Best-effort “domain” (DNS cache + HTTP Host + TLS SNI)

Để tab Capture hiển thị domain/host “thân thiện”, dự án làm thêm:

- **DNS response parse** (UDP/53) để map IPv4 → domain (cache nhỏ 128 entry)
- **HTTP Host header** heuristic cho TCP/80/8080
- **TLS SNI** heuristic cho TCP/443 (ClientHello)

Lưu ý khi trình bày:

- Đây là parsing “best-effort” chỉ để demo UI, không phải parser đầy đủ.

---

### 6.10 Output capture được format như nào để TUI parse

`generate_capture()` (ở `rtl8188_cmd.c`) dump ring ra text:

- header line bắt đầu bằng `[...]`
- dòng `ADDR:...`
- nếu parse được chat_frame:
  - `CF_VER`, `CF_TYPE`, `CF_PLEN`, rồi `IV:`, `HMAC1:`, `HMAC2:`, `ENC1:`, `ENC2:`
- nếu không:
  - `DATA:` hex dump
- cuối cùng: `TOTAL:<count>`

TUI parse bằng prefix để tách danh sách + chi tiết.

