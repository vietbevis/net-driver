/*
 * RTL8188ETV WiFi Companion Monitor — Tab Drawing
 *
 * Vẽ nội dung từng tab trong giao diện TUI:
 *
 *   draw_tab_info()    — F1: Thông tin USB device, MAC, uptime
 *   draw_tab_scan()    — F2: Bảng danh sách AP với tín hiệu màu
 *   draw_tab_status()  — F3: Trạng thái kết nối hiện tại
 *   draw_tab_monitor() — F4: Thống kê gói + bar chart L2/L4
 *   draw_tab_capture() — F5: Deep packet list + detail panel
 *   draw_tab_connect() — F6: Form nhập SSID + password
 *
 * Mỗi hàm nhận (top, bot, cols) — vùng content area trong terminal.
 */

#include "rtl8188_cli.h"

/* ================================================================
 * Hàm phụ trợ nội bộ
 * ================================================================ */

/**
 * strnstr_wrap() - Tìm chuỗi needle trong hay với giới hạn độ dài
 * @hay:    Chuỗi nguồn
 * @needle: Chuỗi cần tìm
 * @len:    Số byte tối đa trong hay để tìm kiếm
 *
 * Trả về: con trỏ đến vị trí đầu tiên tìm thấy, hoặc NULL.
 */
static const char *strnstr_wrap(const char *hay, const char *needle, int len)
{
	int nlen = strlen(needle);
	int i;

	if (nlen > len)
		return NULL;
	for (i = 0; i <= len - nlen; i++)
		if (strncmp(hay + i, needle, nlen) == 0)
			return hay + i;
	return NULL;
}

/**
 * freq_to_channel() - Chuyển đổi tần số MHz sang số kênh WiFi
 * @freq: Tần số MHz (vd: 2412, 5180)
 *
 * Hỗ trợ băng tần 2.4GHz (kênh 1-14) và 5GHz (kênh 36-165).
 *
 * Trả về: số kênh, hoặc 0 nếu không nhận dạng được.
 */
static int freq_to_channel(int freq)
{
	if (freq == 2484)
		return 14;
	if (freq >= 2412 && freq <= 2472)
		return (freq - 2412) / 5 + 1;
	if (freq >= 5180 && freq <= 5825)
		return (freq - 5000) / 5;
	return 0;
}

/* ================================================================
 * Parse iw scan output
 * ================================================================ */

/**
 * parse_scan_results() - Phân tích raw output của lệnh iw scan
 * @raw: Chuỗi output từ `iw dev <iface> scan`
 *
 * Tìm các khối "BSS xx:xx:xx..." và trích xuất:
 *   - BSSID, SSID, channel (qua freq), signal (dBm), associated flag
 *
 * Kết quả lưu vào g_aps[] và g_ap_count.
 */
void parse_scan_results(const char *raw)
{
	const char *p = raw;
	const char *next;

	g_ap_count = 0;

	while ((p = strstr(p, "BSS ")) != NULL && g_ap_count < MAX_APS) {
		struct ap_entry *ap = &g_aps[g_ap_count];
		int section;

		memset(ap, 0, sizeof(*ap));

		/* Đọc BSSID (17 ký tự MAC) ngay sau "BSS " */
		if (sscanf(p + 4, "%17s", ap->bssid) != 1) {
			p++;
			continue;
		}

		/* Kiểm tra "-- associated" trên cùng dòng với BSS */
		{
			const char *eol = strchr(p, '\n');
			if (eol && strnstr_wrap(p, "associated", eol - p))
				ap->associated = 1;
		}

		/* Xác định phạm vi của khối BSS này (đến BSS tiếp theo) */
		next    = strstr(p + 4, "\nBSS ");
		section = next ? (int)(next - p) : (int)strlen(p);

		/* Trích tần số → kênh */
		{
			const char *f = strnstr_wrap(p, "freq: ", section);
			if (f)
				ap->channel = freq_to_channel(atoi(f + 6));
		}

		/* Trích cường độ tín hiệu dBm */
		{
			const char *s = strnstr_wrap(p, "signal: ", section);
			if (s)
				ap->signal = atoi(s + 8);
		}

		/* Trích SSID (đến cuối dòng) */
		{
			const char *s = strnstr_wrap(p, "SSID: ", section);
			if (s) {
				const char *end = strchr(s + 6, '\n');
				int len = end ? (int)(end - s - 6) : 0;
				if (len > 63) len = 63;
				memcpy(ap->ssid, s + 6, len);
				ap->ssid[len] = '\0';
			}
		}

		g_ap_count++;
		p += next ? (next - p) : section;
	}
}

/* ================================================================
 * Parse capture output
 * ================================================================ */

/**
 * parse_capture_output() - Phân tích output lệnh "capture" thành g_caplist[]
 * @buf: Buffer chứa output từ generate_capture() của kernel module
 *
 * Format kernel output:
 *   [N] Xs | Proto | NB [CHAT-AES]   <- nhận biết bằng '[' + digit
 *   ADDR:ip:port->ip:port
 *   CF_VER:1 / CF_TYPE:0x04(CHAT) / CF_PLEN:N
 *   IV:xx xx ...
 *   HMAC1:xx xx ...  / HMAC2:xx xx ...
 *   ENC1:xx xx ...   / ENC2:xx xx ...
 *   DATA:xx xx ...   (gói thông thường)
 */
void parse_capture_output(const char *buf)
{
	static char tmp[BUF_SIZE];
	char *line, *save;
	int pkt = -1;
	int len;

	g_caplist_count = 0;
	len = g_resp_len < BUF_SIZE - 1 ? g_resp_len : BUF_SIZE - 2;
	memcpy(tmp, buf, len);
	tmp[len] = '\0';

	line = strtok_r(tmp, "\n", &save);
	while (line) {
		/* Dòng bắt đầu gói mới: '[' theo sau bởi chữ số */
		if (line[0] == '[' && isdigit((unsigned char)line[1])) {
			pkt++;
			if (pkt >= MAX_CAP_PKTS) break;
			g_caplist_count = pkt + 1;
			memset(&g_caplist[pkt], 0, sizeof(g_caplist[pkt]));
			strncpy(g_caplist[pkt].hdr, line,
				sizeof(g_caplist[pkt].hdr) - 1);
			g_caplist[pkt].is_chat =
				strstr(line, "[CHAT-AES]") ? 1 : 0;

		} else if (pkt >= 0) {
			int nd = g_caplist[pkt].ndetail;

			if (strncmp(line, "ADDR:", 5) == 0) {
				/* Địa chỉ nguồn/đích */
				strncpy(g_caplist[pkt].addr, line + 5,
					sizeof(g_caplist[pkt].addr) - 1);

			} else if (nd < MAX_DET_LINES &&
				   (strncmp(line, "CF_",   3) == 0 ||
				    strncmp(line, "IV:",   3) == 0 ||
				    strncmp(line, "HMAC1:", 6) == 0 ||
				    strncmp(line, "HMAC2:", 6) == 0 ||
				    strncmp(line, "ENC1:", 5) == 0 ||
				    strncmp(line, "ENC2:", 5) == 0 ||
				    strncmp(line, "DATA:", 5) == 0)) {
				/* Dòng detail của gói */
				strncpy(g_caplist[pkt].detail[nd], line,
					sizeof(g_caplist[pkt].detail[nd]) - 1);
				g_caplist[pkt].ndetail++;
			}
		}
		line = strtok_r(NULL, "\n", &save);
	}
}

/* ================================================================
 * Tab F1: Info
 * ================================================================ */

/**
 * draw_tab_info() - Vẽ thông tin USB device (F1)
 *
 * Lấy output lệnh "info" từ kernel và in từng dòng với màu tương ứng.
 */
void draw_tab_info(int top, int bot, int cols)
{
	int y;
	char *line;

	g_resp_len = dev_command("info", g_resp, BUF_SIZE);
	if (g_resp_len <= 0) {
		mvprintw(top + 2, 3, "Không thể giao tiếp với /dev/rtl8188");
		return;
	}

	y    = top + 1;
	line = strtok(g_resp, "\n");
	while (line && y < bot) {
		if (strstr(line, "===="))
			attron(COLOR_PAIR(CP_HEADER) | A_BOLD);
		else if (strstr(line, "CONNECTED"))
			attron(COLOR_PAIR(CP_CONNECTED) | A_BOLD);
		else if (strstr(line, "NOT FOUND"))
			attron(COLOR_PAIR(CP_ERROR) | A_BOLD);

		mvprintw(y, 2, "%-*.*s", cols - 4, cols - 4, line);
		attroff(A_BOLD);
		attrset(COLOR_PAIR(CP_NORMAL));
		y++;
		line = strtok(NULL, "\n");
	}
}

/* ================================================================
 * Tab F2: Scan
 * ================================================================ */

/**
 * draw_tab_scan() - Vẽ bảng danh sách AP WiFi (F2)
 *
 * Hiển thị bảng với cột: No, SSID, BSSID, Channel, Signal.
 * Màu sắc theo cường độ tín hiệu:
 *   xanh lá  ≥ -50 dBm, vàng ≥ -70 dBm, đỏ < -70 dBm
 * AP đang kết nối: xanh lá bold + "<<< connected"
 */
void draw_tab_scan(int top, int bot, int cols)
{
	int y, i, visible;

	if (g_ap_count == 0) {
		attron(COLOR_PAIR(CP_TABLE_HDR));
		mvprintw(top + 2, 3, "%-*s", cols - 6,
			 "Nhấn 's' để quét WiFi");
		attroff(COLOR_PAIR(CP_TABLE_HDR));
		return;
	}

	/* Tiêu đề bảng */
	attron(COLOR_PAIR(CP_TABLE_HDR) | A_BOLD);
	mvprintw(top + 1, 2, "%-4s %-20s %-18s %-4s %-8s %s",
		 "No", "SSID", "BSSID", "CH", "SIGNAL", "");
	mvhline(top + 2, 2, ACS_HLINE, cols - 4);
	attroff(COLOR_PAIR(CP_TABLE_HDR) | A_BOLD);

	/* Điều chỉnh vị trí cuộn */
	visible = bot - top - 4;
	if (g_scan_scroll > g_ap_count - visible && g_ap_count > visible)
		g_scan_scroll = g_ap_count - visible;
	if (g_scan_scroll < 0)
		g_scan_scroll = 0;

	/* Vẽ từng dòng AP */
	y = top + 3;
	for (i = g_scan_scroll; i < g_ap_count && y < bot - 1; i++, y++) {
		struct ap_entry *ap = &g_aps[i];
		int pair;

		if (ap->associated)
			pair = CP_CONNECTED;
		else if (ap->signal >= -50)
			pair = CP_SIGNAL_HI;
		else if (ap->signal >= -70)
			pair = CP_SIGNAL_MED;
		else
			pair = CP_SIGNAL_LO;

		attron(COLOR_PAIR(pair));
		mvprintw(y, 2, "%-4d %-20.20s %-18s %-4d %4d dBm %s",
			 i + 1,
			 ap->ssid[0] ? ap->ssid : "(hidden)",
			 ap->bssid, ap->channel, ap->signal,
			 ap->associated ? " <<< connected" : "");
		attroff(COLOR_PAIR(pair));
	}

	/* Gợi ý cuộn nếu danh sách dài hơn màn hình */
	if (g_ap_count > visible) {
		attron(COLOR_PAIR(CP_MENU));
		mvprintw(bot - 1, 2,
			 " [UP/DOWN: cuộn | %d AP | đang xem %d-%d] ",
			 g_ap_count, g_scan_scroll + 1,
			 g_scan_scroll + visible < g_ap_count
			 ? g_scan_scroll + visible : g_ap_count);
		attroff(COLOR_PAIR(CP_MENU));
	}
}

/* ================================================================
 * Tab F3: Status
 * ================================================================ */

/**
 * draw_tab_status() - Vẽ trạng thái kết nối WiFi hiện tại (F3)
 *
 * Lấy output lệnh "status" từ kernel và in với màu tương ứng.
 * Dòng "Connected to" → xanh lá, "Not connected" → đỏ.
 */
void draw_tab_status(int top, int bot, int cols)
{
	int y;
	char *line;

	g_resp_len = dev_command("status", g_resp, BUF_SIZE);
	if (g_resp_len <= 0) {
		mvprintw(top + 2, 3, "Không có dữ liệu");
		return;
	}

	y    = top + 1;
	line = strtok(g_resp, "\n");
	while (line && y < bot) {
		if (strstr(line, "==="))
			attron(COLOR_PAIR(CP_HEADER) | A_BOLD);
		else if (strstr(line, "Connected to"))
			attron(COLOR_PAIR(CP_CONNECTED) | A_BOLD);
		else if (strstr(line, "Not connected"))
			attron(COLOR_PAIR(CP_ERROR));
		else if (strstr(line, "SSID:") || strstr(line, "signal:"))
			attron(A_BOLD);

		mvprintw(y, 2, "%-*.*s", cols - 4, cols - 4, line);
		attrset(COLOR_PAIR(CP_NORMAL));
		y++;
		line = strtok(NULL, "\n");
	}
}

/* ================================================================
 * Tab F4: Monitor
 * ================================================================ */

/**
 * draw_tab_monitor() - Vẽ thống kê gói tin real-time với bar chart (F4)
 *
 * Parse output lệnh "stats" để lấy bộ đếm, sau đó vẽ:
 *   - Text stats (monitor on/off, RX packets, bytes...)
 *   - Bar chart L2: ARP/IPv4/IPv6/Other
 *   - Bar chart L4: TCP/UDP/ICMP
 *   - Bar chart port filter (nếu active)
 *   - Đếm CryptoChat nếu có
 */
void draw_tab_monitor(int top, int bot, int cols)
{
	int y, bar_w;
	char *line;
	int arp = 0, ip4 = 0, ip6 = 0, oth = 0, total_l2;
	int tcp = 0, udp = 0, icmp = 0, chat = 0, total_l4;
	int filter_port_val = 0, filter_matched = 0;

	g_resp_len = dev_command("stats", g_resp, BUF_SIZE);
	if (g_resp_len <= 0) {
		mvprintw(top + 2, 3, "Không có dữ liệu");
		return;
	}

	/* Parse các giá trị từ output "stats" */
	{
		const char *p;
		p = strstr(g_resp, "ARP:");         if (p) arp  = atoi(p + 4);
		p = strstr(g_resp, "IPv4:");        if (p) ip4  = atoi(p + 5);
		p = strstr(g_resp, "IPv6:");        if (p) ip6  = atoi(p + 5);
		p = strstr(g_resp, "Other:");       if (p) oth  = atoi(p + 6);
		p = strstr(g_resp, "TCP:");         if (p) tcp  = atoi(p + 4);
		p = strstr(g_resp, "UDP:");         if (p) udp  = atoi(p + 4);
		p = strstr(g_resp, "ICMP:");        if (p) icmp = atoi(p + 5);
		p = strstr(g_resp, "Chat(9090):");  if (p) chat = atoi(p + 11);
		p = strstr(g_resp, "[Filter: port ");
		if (p) {
			filter_port_val = atoi(p + 14);
			p = strstr(p, "Matched pkts:");
			if (p) filter_matched = atoi(p + 13);
		}
	}
	total_l2 = arp + ip4 + ip6 + oth;
	total_l4 = tcp + udp + icmp;

	/* In text stats với màu sắc */
	y = top + 1;
	{
		char resp_copy[BUF_SIZE];
		strncpy(resp_copy, g_resp, BUF_SIZE - 1);
		resp_copy[BUF_SIZE - 1] = '\0';

		line = strtok(resp_copy, "\n");
		while (line && y < bot - 14) {
			if (strstr(line, "===="))
				attron(COLOR_PAIR(CP_HEADER) | A_BOLD);
			else if (strstr(line, "[Filter: port") ||
				 strstr(line, "Matched pkts:"))
				attron(COLOR_PAIR(CP_BAR_CHAT) | A_BOLD);
			else if (strstr(line, "Chat(9090)"))
				attron(COLOR_PAIR(CP_ERROR) | A_BOLD);
			else if (strstr(line, "ON"))
				attron(COLOR_PAIR(CP_CONNECTED));
			else if (strstr(line, "OFF"))
				attron(COLOR_PAIR(CP_ERROR));

			mvprintw(y, 2, "%-*.*s", cols - 4, cols - 4, line);
			attrset(COLOR_PAIR(CP_NORMAL));
			y++;
			line = strtok(NULL, "\n");
		}
	}

	/* Độ rộng thanh bar (trừ phần nhãn bên trái) */
	bar_w = cols - 30;
	if (bar_w < 10) bar_w = 10;

	/* Bar chart L2 */
	if (total_l2 > 0) {
		y++;
		attron(COLOR_PAIR(CP_TABLE_HDR) | A_BOLD);
		mvprintw(y++, 2, "L2 Protocol Distribution:");
		attroff(A_BOLD);
		y++;

		mvprintw(y, 2, "  ARP  %6d ", arp);
		draw_bar(y++, 17, bar_w, arp, total_l2, CP_BAR_ARP);
		mvprintw(y, 2, "  IPv4 %6d ", ip4);
		draw_bar(y++, 17, bar_w, ip4, total_l2, CP_BAR_IP4);
		mvprintw(y, 2, "  IPv6 %6d ", ip6);
		draw_bar(y++, 17, bar_w, ip6, total_l2, CP_BAR_IP6);
		mvprintw(y, 2, "  Oth  %6d ", oth);
		draw_bar(y++, 17, bar_w, oth, total_l2, CP_BAR_OTH);
	}

	/* Bar chart L4 */
	if (total_l4 > 0) {
		y++;
		attron(COLOR_PAIR(CP_TABLE_HDR) | A_BOLD);
		mvprintw(y++, 2, "L4 Protocol Distribution:");
		attroff(A_BOLD);
		y++;

		mvprintw(y, 2, "  TCP  %6d ", tcp);
		draw_bar(y++, 17, bar_w, tcp, total_l4, CP_BAR_TCP);
		mvprintw(y, 2, "  UDP  %6d ", udp);
		draw_bar(y++, 17, bar_w, udp, total_l4, CP_BAR_UDP);
		mvprintw(y, 2, "  ICMP %6d ", icmp);
		draw_bar(y++, 17, bar_w, icmp, total_l4, CP_BAR_ICMP);
	}

	/* Bar chart port filter (nếu đang active) */
	if (filter_port_val > 0 && y < bot - 3) {
		int ref = total_l4 > 0 ? total_l4
			: (filter_matched > 0 ? filter_matched : 1);
		y++;
		attron(COLOR_PAIR(CP_TABLE_HDR) | A_BOLD);
		mvprintw(y++, 2, "Port Filter:");
		attroff(A_BOLD);
		y++;
		mvprintw(y, 2, "  :%d %5d ", filter_port_val, filter_matched);
		draw_bar(y++, 17, bar_w, filter_matched, ref, CP_BAR_CHAT);
	} else if (chat > 0 && y < bot - 2) {
		y++;
		attron(COLOR_PAIR(CP_BAR_CHAT) | A_BOLD);
		mvprintw(y, 2, "  ** CryptoChat(9090): %d packets **", chat);
		attroff(COLOR_PAIR(CP_BAR_CHAT) | A_BOLD);
	}

	/* Gợi ý phím ở dòng dưới cùng */
	attron(COLOR_PAIR(CP_MENU));
	if (g_filter_port > 0)
		mvprintw(bot - 1, 2,
			 " [2s refresh | 'm' monitor | 'f' filter (port %d) | 'F' xóa filter | 'r' refresh] ",
			 g_filter_port);
	else
		mvprintw(bot - 1, 2,
			 " [2s refresh | 'm' monitor | 'f' đặt filter | 'r' refresh] ");
	attroff(COLOR_PAIR(CP_MENU));
}

/* ================================================================
 * Tab F5: Capture — Split pane: danh sách + chi tiết
 * ================================================================ */

/**
 * draw_capture_list() - Vẽ danh sách gói tin trong panel trên
 * @top, @bot: Phạm vi dòng
 * @cols:      Số cột
 * @sel:       Index gói đang được chọn (highlighted)
 */
static void draw_capture_list(int top, int bot, int cols, int sel)
{
	int y     = top;
	int visible = bot - top;
	int start = 0;
	int i;

	/* Cuộn để giữ item đang chọn trong tầm nhìn */
	if (sel >= visible)
		start = sel - visible + 1;

	for (i = start; i < g_caplist_count && y < bot; i++, y++) {
		struct cap_pkt *p = &g_caplist[i];
		int attr;

		if (i == sel)
			attr = COLOR_PAIR(CP_MENU_SEL) | A_BOLD;
		else if (p->is_chat)
			attr = COLOR_PAIR(CP_ERROR);
		else
			attr = COLOR_PAIR(CP_NORMAL);

		attron(attr);
		mvprintw(y, 1, "%c %-*.*s",
			 i == sel ? '>' : ' ',
			 cols - 4, cols - 4, p->hdr);
		attroff(attr);
	}

	if (g_caplist_count == 0) {
		attron(COLOR_PAIR(CP_SIGNAL_LO));
		mvprintw(top + 1, 3, "(chưa bắt gói nào)");
		attroff(COLOR_PAIR(CP_SIGNAL_LO));
	}
}

/**
 * draw_capture_detail() - Vẽ chi tiết gói đang chọn trong panel dưới
 * @top, @bot: Phạm vi dòng của panel dưới
 * @cols:      Số cột
 * @sel:       Index gói đang chọn
 *
 * Hiển thị địa chỉ và các trường chat_frame với màu riêng:
 *   IV    → xanh lá,  HMAC1/2 → tím,  ENC1/2 → đỏ (ciphertext)
 */
static void draw_capture_detail(int top, int bot, int cols, int sel)
{
	struct cap_pkt *p;
	int y = top;
	int w = cols - 6;
	int i;

	if (w < 10) w = 10;

	if (g_caplist_count == 0 || sel >= g_caplist_count) {
		mvprintw(top, 3, "(chọn gói bằng UP/DOWN)");
		return;
	}

	p = &g_caplist[sel];

	/* Địa chỉ IP:port → IP:port */
	if (p->addr[0] && y < bot) {
		attron(COLOR_PAIR(CP_CONNECTED) | A_BOLD);
		mvprintw(y++, 3, "%-*.*s", w, w, p->addr);
		attroff(A_BOLD);
	}

	/* Các trường detail với màu theo loại */
	for (i = 0; i < p->ndetail && y < bot; i++) {
		const char *d = p->detail[i];
		int attr;

		if (strncmp(d, "CF_TYPE:", 8) == 0)
			attr = COLOR_PAIR(CP_SIGNAL_HI)  | A_BOLD;
		else if (strncmp(d, "CF_", 3) == 0)
			attr = COLOR_PAIR(CP_TABLE_HDR);
		else if (strncmp(d, "IV:", 3) == 0)
			attr = COLOR_PAIR(CP_BAR_ICMP)   | A_BOLD;
		else if (strncmp(d, "HMAC1:", 6) == 0 ||
			 strncmp(d, "HMAC2:", 6) == 0)
			attr = COLOR_PAIR(CP_BAR_UDP)    | A_BOLD;
		else if (strncmp(d, "ENC1:", 5) == 0 ||
			 strncmp(d, "ENC2:", 5) == 0)
			attr = COLOR_PAIR(CP_BAR_CHAT)   | A_BOLD;  /* đỏ = ciphertext */
		else
			attr = COLOR_PAIR(CP_NORMAL);

		attron(attr);
		mvprintw(y++, 3, "%-*.*s", w, w, d);
		attroff(attr);
	}

	if (p->ndetail == 0 && y < bot) {
		attron(COLOR_PAIR(CP_SIGNAL_LO));
		mvprintw(y, 3, "(không có dữ liệu chi tiết)");
		attroff(COLOR_PAIR(CP_SIGNAL_LO));
	}
}

/**
 * draw_tab_capture() - Vẽ tab Deep Packet Capture (F5)
 *
 * Chia màn hình thành 2 panel (55% / 45%):
 *   Panel trên: danh sách gói bắt được
 *   Panel dưới: chi tiết gói đang chọn (địa chỉ + trường crypto)
 */
void draw_tab_capture(int top, int bot, int cols)
{
	int split, sel;

	/* Lấy và parse dữ liệu capture mới nhất */
	g_resp_len = dev_command("capture", g_resp, BUF_SIZE);
	if (g_resp_len > 0)
		parse_capture_output(g_resp);

	/* Giới hạn con trỏ chọn trong phạm vi hợp lệ */
	sel = g_cap_scroll;
	if (g_caplist_count > 0 && sel >= g_caplist_count)
		sel = g_caplist_count - 1;
	if (sel < 0) sel = 0;
	g_cap_scroll = sel;

	/* Vị trí đường phân cách: 55% chiều cao vùng content */
	split = top + (bot - top) * 55 / 100;

	/* Panel danh sách gói */
	draw_capture_list(top + 1, split, cols, sel);

	/* Đường phân cách với tiêu đề */
	attron(COLOR_PAIR(CP_TABLE_HDR) | A_BOLD);
	mvhline(split, 0, ACS_HLINE, cols);
	if (g_caplist_count > 0 && sel < g_caplist_count) {
		char title[48];
		snprintf(title, sizeof(title), "[ Gói %d - Chi tiết ]", sel);
		mvprintw(split, (cols - (int)strlen(title)) / 2, "%s", title);
	}
	attroff(COLOR_PAIR(CP_TABLE_HDR) | A_BOLD);

	/* Panel chi tiết gói */
	draw_capture_detail(split + 1, bot - 1, cols, sel);

	/* Gợi ý phím ở dòng cuối */
	attron(COLOR_PAIR(CP_MENU));
	if (g_filter_port > 0)
		mvprintw(bot - 1, 1,
			 " UP/DN:chọn | r:refresh | f:filter(%d) | F:xóa"
			 " | IV=lá HMAC=tím ENC=đỏ(mã hóa) ",
			 g_filter_port);
	else
		mvprintw(bot - 1, 1,
			 " UP/DN:chọn | r:refresh | f:đặt filter"
			 " | IV=lá HMAC=tím ENC=đỏ(ciphertext) ");
	attroff(COLOR_PAIR(CP_MENU));
}

/* ================================================================
 * Tab F6: Connect — Form nhập SSID + password
 * ================================================================ */

/**
 * draw_tab_connect() - Vẽ form kết nối WiFi (F6)
 *
 * Hiển thị hộp form với 2 trường: SSID và Password.
 * Trường đang active được underline và dùng CP_INPUT màu.
 * Password được ẩn bằng '*'.
 */
void draw_tab_connect(int top, int bot, int cols)
{
	int mid = (cols - 50) / 2;

	(void)bot;

	if (mid < 2) mid = 2;

	draw_box(top + 2, mid, 10, 50, "Kết nối WiFi");

	attron(COLOR_PAIR(CP_NORMAL));
	mvprintw(top + 4, mid + 3, "SSID:");
	mvprintw(top + 6, mid + 3, "Mật khẩu:");
	mvprintw(top + 9, mid + 3, "[Enter] Kết nối  [Tab] Chuyển trường");
	mvprintw(top + 10, mid + 3, "Chờ kết quả (khoảng 20-60 giây)");

	/* Trường SSID */
	if (g_connect_field == 0)
		attron(COLOR_PAIR(CP_INPUT) | A_UNDERLINE);
	mvprintw(top + 4, mid + 14, "%-30.30s", g_ssid);
	attroff(A_UNDERLINE);

	/* Trường Password (hiển thị dấu *) */
	attron(g_connect_field == 1
	       ? COLOR_PAIR(CP_INPUT) | A_UNDERLINE
	       : COLOR_PAIR(CP_NORMAL));
	{
		char masked[128];
		size_t plen = strlen(g_pass);
		size_t i;
		for (i = 0; i < plen && i < 30; i++)
			masked[i] = '*';
		masked[i] = '\0';
		mvprintw(top + 6, mid + 14, "%-30.30s", masked);
	}
	attroff(A_UNDERLINE);
	attrset(COLOR_PAIR(CP_NORMAL));

	/* Thông báo kết quả kết nối */
	if (g_connect_msg[0]) {
		if (strstr(g_connect_msg, "ERROR") ||
		    strstr(g_connect_msg, "fail"))
			attron(COLOR_PAIR(CP_ERROR) | A_BOLD);
		else
			attron(COLOR_PAIR(CP_CONNECTED) | A_BOLD);
		mvprintw(top + 11, mid + 2, "%.48s", g_connect_msg);
		attroff(A_BOLD);
		attrset(COLOR_PAIR(CP_NORMAL));
	}
}

/**
 * handle_connect_input() - Xử lý phím trong tab Connect
 * @ch: Ký tự/mã phím nhận được từ getch()
 *
 * Tab/Up/Down: chuyển giữa 2 trường
 * Backspace: xóa ký tự cuối
 * Enter: gửi lệnh connect đến kernel module (block đến khi có kết quả)
 * Ký tự in được: thêm vào trường đang active
 */
void handle_connect_input(int ch)
{
	char *field  = g_connect_field == 0 ? g_ssid : g_pass;
	int   maxlen = g_connect_field == 0 ? 63 : 127;
	int   len    = strlen(field);

	if (ch == '\t' || ch == KEY_DOWN || ch == KEY_UP) {
		g_connect_field = 1 - g_connect_field;  /* Toggle 0↔1 */

	} else if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
		if (len > 0)
			field[len - 1] = '\0';

	} else if (ch == '\n' || ch == KEY_ENTER) {
		if (g_ssid[0] == '\0') {
			snprintf(g_connect_msg, sizeof(g_connect_msg),
				 "ERROR: SSID không được để trống");
			return;
		}

		/* Tạo lệnh connect */
		char cmd[256];
		if (g_pass[0])
			snprintf(cmd, sizeof(cmd),
				 "connect %s %s", g_ssid, g_pass);
		else
			snprintf(cmd, sizeof(cmd), "connect %s", g_ssid);

		snprintf(g_connect_msg, sizeof(g_connect_msg),
			 "Đang kết nối đến %s ...", g_ssid);
		refresh();

		/* Block chờ kết quả từ kernel (connect_work_fn) */
		g_resp_len = dev_command(cmd, g_resp, BUF_SIZE);
		if (g_resp_len > 0) {
			char *nl   = strchr(g_resp, '\n');
			int   clen = nl ? (int)(nl - g_resp) : g_resp_len;
			if (clen > 200) clen = 200;
			snprintf(g_connect_msg, sizeof(g_connect_msg),
				 "%.*s", clen, g_resp);
		} else {
			snprintf(g_connect_msg, sizeof(g_connect_msg),
				 "ERROR: Không nhận được phản hồi từ module");
		}

	} else if (isprint(ch) && len < maxlen) {
		field[len]     = ch;
		field[len + 1] = '\0';
	}
}