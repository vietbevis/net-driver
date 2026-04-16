// SPDX-License-Identifier: GPL-2.0
/*
 * RTL8188ETV WiFi Companion Monitor — Command Handlers
 *
 * Gồm hai nhóm hàm:
 *
 * 1. Synchronous generators — được gọi trực tiếp từ rtl8188_dev_write():
 *      generate_info()    — thông tin USB device, MAC, uptime
 *      generate_status()  — trạng thái kết nối WiFi qua iw link
 *      generate_stats()   — thống kê gói tin từ các atomic counter
 *      generate_capture() — dump ring buffer dưới dạng text
 *
 * 2. Async work functions — chạy trên workqueue "rtl8188_mon":
 *      scan_work_fn()       — quét WiFi qua iw scan
 *      connect_work_fn()    — kết nối WiFi (nmcli / wpa_supplicant / iw)
 *      disconnect_work_fn() — ngắt kết nối WiFi
 *
 * Tất cả output được ghi vào g_mon->resp_buf và set_resp() / trực tiếp
 * đặt resp_ready=true rồi wake_up_interruptible() để unblock read().
 */

#include "rtl8188_mon.h"
#include <linux/slab.h>
#include <linux/rtnetlink.h>

/* Ensure mon->ifname/ndev is up-to-date (handles rename/replug). */
static void ensure_iface_present(struct rtl8188_mon *mon)
{
	struct net_device *dev;

	if (!mon)
		return;

	/* Fast path: ndev exists and matches cached name. */
	if (mon->ndev && mon->ifname[0] &&
	    strncmp(mon->ndev->name, mon->ifname, IFNAMSIZ) == 0)
		return;

	/* Validate cached ifname still exists. */
	if (mon->ifname[0]) {
		rtnl_lock();
		dev = dev_get_by_name(&init_net, mon->ifname);
		if (dev) {
			mon->ndev = dev;
			dev_put(dev);
			rtnl_unlock();
			return;
		}
		rtnl_unlock();
	}

	/* Fallback to full rescan for our interface. */
	scan_existing_netdev();
}

/* ================================================================
 * Hàm tiện ích nội bộ
 * ================================================================ */

/**
 * chat_type_name() - Trả về tên string của loại message CryptoChat
 * @type: Giá trị MSG_TYPE_* từ chat_frame header
 */
static const char *chat_type_name(u8 type)
{
	switch (type) {
	case MSG_TYPE_AUTH:      return "AUTH";
	case MSG_TYPE_AUTH_OK:   return "AUTH_OK";
	case MSG_TYPE_AUTH_FAIL: return "AUTH_FAIL";
	case MSG_TYPE_CHAT:      return "CHAT";
	case MSG_TYPE_SYSTEM:    return "SYSTEM";
	case MSG_TYPE_LOGOUT:    return "LOGOUT";
	case MSG_TYPE_LIST:      return "LIST";
	case MSG_TYPE_BROADCAST: return "BROADCAST";
	case MSG_TYPE_REGISTER:  return "REGISTER";
	case MSG_TYPE_REG_OK:    return "REG_OK";
	case MSG_TYPE_REG_FAIL:  return "REG_FAIL";
	default:                 return "UNKNOWN";
	}
}

/**
 * l4_proto_str() - Trả về tên string của giao thức L4
 * @proto: Giá trị IPPROTO_*
 */
static const char *l4_proto_str(u8 proto)
{
	switch (proto) {
	case IPPROTO_TCP:  return "TCP";
	case IPPROTO_UDP:  return "UDP";
	case IPPROTO_ICMP: return "ICMP";
	default:           return "---";
	}
}

/**
 * hex_dump_line() - In tối đa 16 byte hex vào buffer, nhóm 8+8
 * @buf:     Buffer đích
 * @bufsize: Kích thước buffer còn lại
 * @data:    Dữ liệu cần dump
 * @len:     Số byte thực tế của data
 *
 * Format: "xx xx xx xx xx xx xx xx  xx xx xx xx xx xx xx xx ..."
 * Thêm "..." nếu len > 16.
 *
 * Trả về: Số byte đã ghi vào buf.
 */
static int hex_dump_line(char *buf, int bufsize, const u8 *data, int len)
{
	int show = min_t(int, len, 16);
	int i, n = 0;

	for (i = 0; i < show && n < bufsize - 4; i++) {
		/* Thêm khoảng cách giữa hai nhóm 8 byte */
		if (i == 8)
			n += snprintf(buf + n, bufsize - n, " ");
		n += snprintf(buf + n, bufsize - n, "%02x ", data[i]);
	}
	if (len > 16)
		n += snprintf(buf + n, bufsize - n, "...");
	return n;
}

/* ================================================================
 * Synchronous generators
 * ================================================================ */

/**
 * generate_info() - Tạo thông tin chi tiết về USB device RTL8188ETV
 * @mon: Trạng thái module
 *
 * Bao gồm: vendor/product ID, USB speed, số endpoint, interface name, MAC.
 * Gọi trực tiếp từ write() handler (process context).
 */
void generate_info(struct rtl8188_mon *mon)
{
	int n = 0;

	mutex_lock(&mon->cmd_lock);

	n += snprintf(mon->resp_buf + n, RESP_BUF_SIZE - n,
		      "============================================\n"
		      "   RTL8188ETV USB WiFi Device Information\n"
		      "============================================\n\n");

	if (mon->dev_present && mon->udev) {
		struct usb_device *u = mon->udev;

		n += snprintf(mon->resp_buf + n, RESP_BUF_SIZE - n,
			      "  Status:       CONNECTED\n"
			      "  Vendor:       0x%04x (%s)\n"
			      "  Product:      0x%04x (%s)\n"
			      "  USB Speed:    %s\n",
			      le16_to_cpu(u->descriptor.idVendor),
			      u->manufacturer ? u->manufacturer : "Realtek",
			      le16_to_cpu(u->descriptor.idProduct),
			      u->product ? u->product : "RTL8188ETV",
			      usb_speed_string(u->speed));

		/* Liệt kê các endpoint của interface đang active */
		if (u->actconfig) {
			struct usb_host_interface *iface =
				u->actconfig->interface[0]->cur_altsetting;
			int i;

			n += snprintf(mon->resp_buf + n, RESP_BUF_SIZE - n,
				      "  Endpoints:    %d\n",
				      iface->desc.bNumEndpoints);

			for (i = 0; i < iface->desc.bNumEndpoints; i++) {
				struct usb_endpoint_descriptor *ep =
					&iface->endpoint[i].desc;
				n += snprintf(mon->resp_buf + n,
					      RESP_BUF_SIZE - n,
					      "    EP%d: %s %s (max %d)\n",
					      ep->bEndpointAddress & 0x0f,
					      usb_endpoint_dir_in(ep) ? "IN " : "OUT",
					      usb_ep_type_string(usb_endpoint_type(ep)),
					      le16_to_cpu(ep->wMaxPacketSize));
			}
		}

		n += snprintf(mon->resp_buf + n, RESP_BUF_SIZE - n,
			      "  Interface:    %s\n",
			      mon->ifname[0] ? mon->ifname : "(none)");

		if (mon->ndev)
			n += snprintf(mon->resp_buf + n, RESP_BUF_SIZE - n,
				      "  MAC Address:  %pM\n",
				      mon->ndev->dev_addr);
	} else {
		n += snprintf(mon->resp_buf + n, RESP_BUF_SIZE - n,
			      "  Status:       NOT FOUND\n"
			      "  Hãy cắm USB WiFi RTL8188ETV vào.\n");
	}

	n += snprintf(mon->resp_buf + n, RESP_BUF_SIZE - n,
		      "\n  Module uptime: %lu giây\n",
		      (jiffies - mon->load_jiffies) / HZ);

	mon->resp_len   = n;
	mon->resp_ready = true;
	mutex_unlock(&mon->cmd_lock);
	wake_up_interruptible(&mon->resp_wq);
}

/**
 * generate_status() - Lấy trạng thái kết nối WiFi hiện tại
 * @mon: Trạng thái module
 *
 * Gọi `iw dev <iface> link` và `ip addr show <iface>` qua shell,
 * đọc kết quả từ file tạm STATUS_FILE và đưa vào resp_buf.
 */
void generate_status(struct rtl8188_mon *mon)
{
	char cmd[256];
	int n;

	ensure_iface_present(mon);
	if (!mon->ifname[0]) {
		set_resp(mon, "Không tìm thấy wireless interface.\n");
		return;
	}

	snprintf(cmd, sizeof(cmd),
		 "{ echo '=== Connection Status ==='; echo; "
		 "/usr/sbin/iw dev %s link 2>&1; echo; "
		 "echo '=== IP Address ==='; "
		 "/usr/sbin/ip addr show %s 2>/dev/null | grep -E 'inet ' || "
		 "echo '  (no IP address)'; } > " STATUS_FILE " 2>&1",
		 mon->ifname, mon->ifname);
	run_cmd(cmd);

	mutex_lock(&mon->cmd_lock);
	n = read_tmpfile(STATUS_FILE, mon->resp_buf, RESP_BUF_SIZE);
	if (n <= 0)
		n = snprintf(mon->resp_buf, RESP_BUF_SIZE,
			     "Không lấy được trạng thái kết nối\n");
	mon->resp_len   = n;
	mon->resp_ready = true;
	mutex_unlock(&mon->cmd_lock);
	wake_up_interruptible(&mon->resp_wq);
}

/**
 * generate_stats() - Tổng hợp thống kê gói tin từ các atomic counter
 * @mon: Trạng thái module
 *
 * Không cần gọi shell — đọc trực tiếp từ atomic_read().
 * Cũng lấy thêm driver statistics qua dev_get_stats() nếu ndev có sẵn.
 */
void generate_stats(struct rtl8188_mon *mon)
{
	struct rtnl_link_stats64 ns;
	int n = 0;
	unsigned long uptime = (jiffies - mon->load_jiffies) / HZ;

	mutex_lock(&mon->cmd_lock);

	n += snprintf(mon->resp_buf + n, RESP_BUF_SIZE - n,
		      "============================================\n"
		      "          Packet Statistics\n"
		      "============================================\n\n");

	n += snprintf(mon->resp_buf + n, RESP_BUF_SIZE - n,
		      "  Monitor:    %s\n"
		      "  Capture:    %s\n\n",
		      mon->pkt_registered ? "ON" : "OFF",
		      mon->capture_on     ? "ON" : "OFF");

	if (mon->pkt_registered) {
		n += snprintf(mon->resp_buf + n, RESP_BUF_SIZE - n,
			      "  [Monitored Traffic]\n"
			      "    RX Packets:  %lld\n"
			      "    RX Bytes:    %lld\n"
			      "    ARP:         %d\n"
			      "    IPv4:        %d\n"
			      "    IPv6:        %d\n"
			      "    Other:       %d\n\n"
			      "  [L4 Protocol]\n"
			      "    TCP:         %d\n"
			      "    UDP:         %d\n"
			      "    ICMP:        %d\n"
			      "    Chat(9090):  %d\n\n",
			      atomic64_read(&mon->rx_pkts),
			      atomic64_read(&mon->rx_bytes),
			      atomic_read(&mon->arp_cnt),
			      atomic_read(&mon->ip_cnt),
			      atomic_read(&mon->ipv6_cnt),
			      atomic_read(&mon->other_cnt),
			      atomic_read(&mon->tcp_cnt),
			      atomic_read(&mon->udp_cnt),
			      atomic_read(&mon->icmp_cnt),
			      atomic_read(&mon->chat_cnt));

		/* Hiển thị thống kê port filter nếu đang active */
		if (mon->filter_port) {
			n += snprintf(mon->resp_buf + n, RESP_BUF_SIZE - n,
				      "  [Filter: port %d]\n"
				      "    Matched pkts: %d\n\n",
				      ntohs(mon->filter_port),
				      atomic_read(&mon->filter_cnt));
		} else {
			n += snprintf(mon->resp_buf + n, RESP_BUF_SIZE - n,
				      "  Filter port:  none (all)\n\n");
		}
	}

	/* Thống kê từ driver (rtl8xxxu) nếu interface đang active */
	if (mon->ndev) {
		dev_get_stats(mon->ndev, &ns);
		n += snprintf(mon->resp_buf + n, RESP_BUF_SIZE - n,
			      "  [Driver Statistics]\n"
			      "    TX: %llu packets, %llu bytes\n"
			      "    RX: %llu packets, %llu bytes\n"
			      "    TX errors: %llu  RX errors: %llu\n"
			      "    TX dropped: %llu  RX dropped: %llu\n\n",
			      ns.tx_packets, ns.tx_bytes,
			      ns.rx_packets, ns.rx_bytes,
			      ns.tx_errors,  ns.rx_errors,
			      ns.tx_dropped, ns.rx_dropped);
	}

	n += snprintf(mon->resp_buf + n, RESP_BUF_SIZE - n,
		      "  Capture buffer: %d / %d entries\n"
		      "  Module uptime:  %lu giây\n",
		      mon->ring_count, CAPTURE_RING_SIZE, uptime);

	mon->resp_len   = n;
	mon->resp_ready = true;
	mutex_unlock(&mon->cmd_lock);
	wake_up_interruptible(&mon->resp_wq);
}

/**
 * generate_capture() - Dump nội dung ring buffer ra text cho userspace đọc
 * @mon: Trạng thái module
 *
 * Format mỗi gói tin:
 *   [N] Xs | Proto/L4 | NB [CHAT-AES]     <- header line (TUI detect bằng '[')
 *   ADDR:src_ip:port->dst_ip:port           <- địa chỉ
 *   CF_VER:1 / CF_TYPE:0x04(CHAT) / ...    <- chat_frame fields (nếu chat_parsed)
 *   IV:xx xx ... / HMAC1:.. / ENC1:..      <- crypto fields
 *   hoặc DATA:xx xx ...                    <- hex dump thông thường
 *
 * Giữ ring_lock trong suốt quá trình đọc để tránh race với packet handler.
 */
void generate_capture(struct rtl8188_mon *mon)
{
	int i, start, count, n = 0;
	unsigned long flags;
	struct pkt_entry *e;
	const char *eth_str;

	mutex_lock(&mon->cmd_lock);
	spin_lock_irqsave(&mon->ring_lock, flags);

	count = mon->ring_count;
	/* Tính vị trí entry cũ nhất trong circular buffer */
	start = (mon->ring_head - count + CAPTURE_RING_SIZE) % CAPTURE_RING_SIZE;

	for (i = 0; i < count && n < RESP_BUF_SIZE - 400; i++) {
		int idx = (start + i) % CAPTURE_RING_SIZE;
		unsigned long age;

		e   = &mon->ring[idx];
		age = (jiffies - e->tstamp) / HZ;

		/* Xác định tên EtherType */
		switch (ntohs(e->eth_proto)) {
		case ETH_P_IP:   eth_str = "IPv4";  break;
		case ETH_P_IPV6: eth_str = "IPv6";  break;
		case ETH_P_ARP:  eth_str = "ARP";   break;
		case ETH_P_PAE:  eth_str = "EAPOL"; break;
		default:         eth_str = "????";  break;
		}

		/*
		 * Dòng header — TUI nhận biết gói mới bằng ký tự '[' ở đầu.
		 * Không thay đổi format này nếu không cập nhật parse_capture_output() trong CLI.
		 */
		n += snprintf(mon->resp_buf + n, RESP_BUF_SIZE - n,
			      "[%d] %lus | %s/%s | %uB",
			      i, age, eth_str, l4_proto_str(e->ip_proto), e->len);
		if (e->domain[0]) {
			const char *scheme = "";
			if (e->ip_proto == IPPROTO_TCP &&
			    (ntohs(e->dst_port) == 443 || ntohs(e->src_port) == 443))
				scheme = "https://";
			n += snprintf(mon->resp_buf + n, RESP_BUF_SIZE - n,
				      " | %s%s", scheme, e->domain);
		}
		if (e->is_chat)
			n += snprintf(mon->resp_buf + n, RESP_BUF_SIZE - n,
				      " [CHAT-AES]");
		n += snprintf(mon->resp_buf + n, RESP_BUF_SIZE - n, "\n");

		/* Dòng địa chỉ: dùng IP nếu có, MAC nếu không */
		if (e->src_ip || e->dst_ip) {
			n += snprintf(mon->resp_buf + n, RESP_BUF_SIZE - n,
				      "ADDR:%pI4:%d->%pI4:%d\n",
				      &e->src_ip, ntohs(e->src_port),
				      &e->dst_ip, ntohs(e->dst_port));
		} else {
			n += snprintf(mon->resp_buf + n, RESP_BUF_SIZE - n,
				      "ADDR:%pM->%pM\n",
				      e->src_mac, e->dst_mac);
		}

		if (e->chat_parsed) {
			/* In chi tiết từng trường của chat_frame */
			n += snprintf(mon->resp_buf + n, RESP_BUF_SIZE - n,
				      "CF_VER:%u\n"
				      "CF_TYPE:0x%02x(%s)\n"
				      "CF_PLEN:%u\n",
				      e->chat_ver, e->chat_type,
				      chat_type_name(e->chat_type),
				      e->chat_plen);

			/* IV: 16 byte, luôn đủ */
			n += snprintf(mon->resp_buf + n, RESP_BUF_SIZE - n, "IV:");
			n += hex_dump_line(mon->resp_buf + n, RESP_BUF_SIZE - n,
					   e->chat_iv, 16);
			n += snprintf(mon->resp_buf + n, RESP_BUF_SIZE - n, "\n");

			/* HMAC: 32 byte, in thành 2 dòng 16 byte để dễ đọc */
			n += snprintf(mon->resp_buf + n, RESP_BUF_SIZE - n, "HMAC1:");
			n += hex_dump_line(mon->resp_buf + n, RESP_BUF_SIZE - n,
					   e->chat_hmac, 16);
			n += snprintf(mon->resp_buf + n, RESP_BUF_SIZE - n, "\n");
			n += snprintf(mon->resp_buf + n, RESP_BUF_SIZE - n, "HMAC2:");
			n += hex_dump_line(mon->resp_buf + n, RESP_BUF_SIZE - n,
					   e->chat_hmac + 16, 16);
			n += snprintf(mon->resp_buf + n, RESP_BUF_SIZE - n, "\n");

			/* Ciphertext: tối đa 32 byte, in 1-2 dòng */
			if (e->chat_enc_len > 0) {
				int enc1 = min_t(int, e->chat_enc_len, 16);
				int enc2 = e->chat_enc_len - enc1;

				n += snprintf(mon->resp_buf + n,
					      RESP_BUF_SIZE - n, "ENC1:");
				n += hex_dump_line(mon->resp_buf + n,
						   RESP_BUF_SIZE - n,
						   e->chat_enc, enc1);
				n += snprintf(mon->resp_buf + n,
					      RESP_BUF_SIZE - n, "\n");

				if (enc2 > 0) {
					n += snprintf(mon->resp_buf + n,
						      RESP_BUF_SIZE - n, "ENC2:");
					n += hex_dump_line(mon->resp_buf + n,
							   RESP_BUF_SIZE - n,
							   e->chat_enc + 16, enc2);
					n += snprintf(mon->resp_buf + n,
						      RESP_BUF_SIZE - n, "\n");
				}
			}
		} else if (e->payload_len > 0) {
			/* Gói thông thường: chỉ in hex dump payload */
			n += snprintf(mon->resp_buf + n, RESP_BUF_SIZE - n, "DATA:");
			n += hex_dump_line(mon->resp_buf + n, RESP_BUF_SIZE - n,
					   e->payload, e->payload_len);
			n += snprintf(mon->resp_buf + n, RESP_BUF_SIZE - n, "\n");
		}
	}

	spin_unlock_irqrestore(&mon->ring_lock, flags);

	if (count == 0)
		n += snprintf(mon->resp_buf + n, RESP_BUF_SIZE - n,
			      "\n  (trống - hãy bật monitor trước: 'monitor on')\n");

	/* Dòng tổng để TUI biết số lượng gói */
	n += snprintf(mon->resp_buf + n, RESP_BUF_SIZE - n, "TOTAL:%d\n", count);

	mon->resp_len   = n;
	mon->resp_ready = true;
	mutex_unlock(&mon->cmd_lock);
	wake_up_interruptible(&mon->resp_wq);
}

/* ================================================================
 * Async work functions (chạy trên workqueue "rtl8188_mon")
 * ================================================================ */

/**
 * scan_work_fn() - Quét WiFi thông qua iw scan
 * @work: work_struct (embedded trong g_mon->scan_work)
 *
 * Thực thi:
 *   ip link set <iface> up
 *   iw dev <iface> scan > /tmp/.rtl8188_scan
 *   (nếu device busy): iw dev <iface> scan dump > /tmp/.rtl8188_scan
 *
 * Kết quả raw iw output được đặt vào resp_buf để TUI parse.
 */
void scan_work_fn(struct work_struct *work)
{
	struct rtl8188_mon *mon = g_mon;
	char cmd[512];
	int n, ret;

	ensure_iface_present(mon);
	if (!mon->ifname[0]) {
		set_resp(mon, "LỖI: Không tìm thấy wireless interface.\n"
			      "Hãy đảm bảo driver rtl8xxxu đã được load "
			      "và thiết bị đã được cắm vào.\n");
		return;
	}

	/*
	 * Thử active scan trước; nếu device đang bận (EBUSY),
	 * dùng scan dump để lấy kết quả đã cache trong firmware.
	 */
	snprintf(cmd, sizeof(cmd),
		 "/usr/sbin/ip link set %s up 2>/dev/null; "
		 "/usr/sbin/iw dev %s scan > " SCAN_FILE " 2>&1 || "
		 "/usr/sbin/iw dev %s scan dump > " SCAN_FILE " 2>&1",
		 mon->ifname, mon->ifname, mon->ifname);
	ret = run_cmd(cmd);

	mutex_lock(&mon->cmd_lock);
	n = read_tmpfile(SCAN_FILE, mon->resp_buf, RESP_BUF_SIZE);
	if (n <= 0)
		n = snprintf(mon->resp_buf, RESP_BUF_SIZE,
			     "Scan thất bại (ret=%d). Kiểm tra lệnh iw đã được cài đặt.\n",
			     ret);
	mon->resp_len   = n;
	mon->resp_ready = true;
	mutex_unlock(&mon->cmd_lock);
	wake_up_interruptible(&mon->resp_wq);
}

/**
 * connect_work_fn() - Kết nối WiFi với chiến lược ưu tiên:
 *   1. NetworkManager (nmcli) — nếu NM đang chạy
 *   2. wpa_cli — nếu wpa_supplicant của NM đang chạy
 *   3. wpa_supplicant riêng — nếu không có gì đang chạy
 *
 * Hỗ trợ: WPA2-PSK, WPA3-SAE (tự phát hiện qua iw scan), Open network.
 * Đợi tối đa 20 giây để link lên trước khi ghi kết quả.
 *
 * @work: work_struct (embedded trong g_mon->connect_work)
 */
void connect_work_fn(struct work_struct *work)
{
	struct rtl8188_mon *mon = g_mon;
	char *cmd;
	int n;

	ensure_iface_present(mon);
	if (!mon->ifname[0]) {
		set_resp(mon, "LỖI: Không tìm thấy wireless interface.\n");
		return;
	}

	/* Cấp phát buffer lớn cho script shell nhiều bước */
	cmd = kzalloc(4096, GFP_KERNEL);
	if (!cmd) {
		set_resp(mon, "LỖI: Hết bộ nhớ kernel.\n");
		return;
	}

	if (mon->cmd_pass[0]) {
		/* Mạng có mật khẩu: thử nmcli → wpa_cli → wpa_supplicant */
		snprintf(cmd, 4096,
			 "IF='%s'; SSID='%s'; PASS='%s';\n"
			 "/usr/sbin/ip link set \"$IF\" up 2>/dev/null; "
			 "/usr/sbin/rfkill unblock all 2>/dev/null || true; "
			 "rm -f " CONNECT_FILE "\n"
			 "echo '=== WPA connect ===' >> " CONNECT_FILE "\n"
			 "echo \"iface: $IF\" >> " CONNECT_FILE "\n"
			 "echo \"ssid:  $SSID\" >> " CONNECT_FILE "\n"
			 "is_sae=0; "
			 "/usr/sbin/iw dev \"$IF\" scan 2>/dev/null | "
			 "awk 'BEGIN{p=0} /^BSS /{p=0} /SSID: /{p=0} "
			 "$0==\"\\tSSID: '\"$SSID\"'\"{p=1} "
			 "p && /Authentication suites: SAE/{print \"SAE\"; exit}' | "
			 "grep -q SAE && is_sae=1; "
			 "echo \"security: $([ $is_sae -eq 1 ] && echo WPA3-SAE || echo WPA2-PSK)\" >> " CONNECT_FILE "\n"
			 "echo '---' >> " CONNECT_FILE "\n"
			 "if /usr/bin/nmcli -t -f RUNNING general 2>/dev/null | grep -q '^running$'; then \n"
			 "  echo 'Using NetworkManager (nmcli)' >> " CONNECT_FILE ";\n"
			 "  /usr/bin/nmcli -t dev set \"$IF\" managed yes >> " CONNECT_FILE " 2>&1 || true;\n"
			 "  /usr/bin/nmcli -t radio wifi on >> " CONNECT_FILE " 2>&1 || true;\n"
			 "  /usr/bin/nmcli -t dev disconnect \"$IF\" >> " CONNECT_FILE " 2>&1 || true;\n"
			 "  CONN=\"$SSID\";\n"
			 "  if /usr/bin/nmcli -t -f NAME connection show 2>/dev/null | grep -Fx \"$CONN\"; then \n"
			 "    echo 'Reusing NM connection profile' >> " CONNECT_FILE ";\n"
			 "  else \n"
			 "    echo 'Creating NM connection profile' >> " CONNECT_FILE ";\n"
			 "    /usr/bin/nmcli -t connection add type wifi ifname \"$IF\" con-name \"$CONN\" ssid \"$CONN\" >> " CONNECT_FILE " 2>&1 || true;\n"
			 "  fi;\n"
			 "  if [ $is_sae -eq 1 ]; then \n"
			 "    /usr/bin/nmcli -t connection modify \"$CONN\" wifi-sec.key-mgmt sae wifi-sec.psk \"$PASS\" wifi-sec.pmf required >> " CONNECT_FILE " 2>&1 || true;\n"
			 "  else \n"
			 "    /usr/bin/nmcli -t connection modify \"$CONN\" wifi-sec.key-mgmt wpa-psk wifi-sec.psk \"$PASS\" wifi-sec.pmf optional >> " CONNECT_FILE " 2>&1 || true;\n"
			 "  fi;\n"
			 "  /usr/bin/nmcli -t connection up \"$CONN\" ifname \"$IF\" >> " CONNECT_FILE " 2>&1 || true;\n"
			 "  /usr/bin/nmcli -t -f GENERAL.STATE,GENERAL.CONNECTION,IP4.ADDRESS,IP4.GATEWAY,IP4.DNS dev show \"$IF\" >> " CONNECT_FILE " 2>&1 || true;\n"
			 "else \n"
			 "  echo 'NM not running; fallback wpa_cli/wpa_supplicant not available in this build path' >> " CONNECT_FILE ";\n"
			 "fi\n"
			 "echo '=== wait for link (up to 20s) ===' >> " CONNECT_FILE "\n"
			 "i=0; while [ $i -lt 20 ]; do "
			 "  /usr/sbin/iw dev \"$IF\" link 2>&1 | tee -a " CONNECT_FILE " | grep -q 'Connected to' && break; "
			 "  sleep 1; i=$((i+1)); "
			 "done\n"
			 "echo '=== ip addr ===' >> " CONNECT_FILE "\n"
			 "/usr/sbin/ip -br addr show \"$IF\" 2>&1 >> " CONNECT_FILE " || true\n"
			 "echo '=== final iw link ===' >> " CONNECT_FILE "\n"
			 "/usr/sbin/iw dev \"$IF\" link 2>&1 >> " CONNECT_FILE,
			 mon->ifname, mon->cmd_ssid, mon->cmd_pass);
	} else {
		/* Mạng mở (Open): dùng iw connect trực tiếp */
		snprintf(cmd, 4096,
			 "/usr/sbin/ip link set %s up 2>/dev/null; "
			 "/usr/sbin/rfkill unblock all 2>/dev/null || true; "
			 "/usr/sbin/iw dev %s connect '%s' > " CONNECT_FILE " 2>&1\n"
			 "echo '=== wait for link (up to 10s) ===' >> " CONNECT_FILE "\n"
			 "i=0; while [ $i -lt 10 ]; do "
			 "  /usr/sbin/iw dev %s link 2>&1 | tee -a " CONNECT_FILE " | grep -q 'Connected to' && break; "
			 "  sleep 1; i=$((i+1)); "
			 "done\n"
			 "echo '=== ip addr ===' >> " CONNECT_FILE "\n"
			 "/usr/sbin/ip -br addr show %s 2>&1 >> " CONNECT_FILE " || true\n"
			 "echo '=== final iw link ===' >> " CONNECT_FILE "\n"
			 "/usr/sbin/iw dev %s link 2>&1 >> " CONNECT_FILE,
			 mon->ifname,
			 mon->ifname, mon->cmd_ssid,
			 mon->ifname,
			 mon->ifname,
			 mon->ifname);
	}

	run_cmd(cmd);
	kfree(cmd);

	/* Tổng hợp kết quả: header thông tin + nội dung CONNECT_FILE */
	mutex_lock(&mon->cmd_lock);
	n  = snprintf(mon->resp_buf, RESP_BUF_SIZE,
		      "Đang kết nối đến '%s'%s...\n\n",
		      mon->cmd_ssid,
		      mon->cmd_pass[0] ? " (WPA)" : " (Open)");
	n += read_tmpfile(CONNECT_FILE, mon->resp_buf + n, RESP_BUF_SIZE - n);
	mon->resp_len   = n;
	mon->resp_ready = true;
	mutex_unlock(&mon->cmd_lock);
	wake_up_interruptible(&mon->resp_wq);
}

/**
 * disconnect_work_fn() - Ngắt kết nối WiFi
 * @work: work_struct (embedded trong g_mon->disconnect_work)
 *
 * Dừng wpa_supplicant nếu đang chạy, sau đó gọi iw disconnect.
 */
void disconnect_work_fn(struct work_struct *work)
{
	struct rtl8188_mon *mon = g_mon;
	char cmd[256];

	ensure_iface_present(mon);
	if (!mon->ifname[0]) {
		set_resp(mon, "LỖI: Không tìm thấy wireless interface.\n");
		return;
	}

	snprintf(cmd, sizeof(cmd),
		 "killall wpa_supplicant 2>/dev/null; "
		 "/usr/sbin/iw dev %s disconnect 2>&1",
		 mon->ifname);
	run_cmd(cmd);
	set_resp(mon, "Đã ngắt kết nối.\n");
}