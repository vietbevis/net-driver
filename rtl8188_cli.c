/*
 * RTL8188ETV WiFi Companion Monitor - TUI Dashboard
 *
 * ncurses-based interface to the rtl8188_mon kernel module.
 * Communicates via /dev/rtl8188 character device.
 *
 * Tabs: F1=Info  F2=Scan  F3=Status  F4=Monitor  F5=Capture  F6=Connect
 *       F10 or q = Quit
 *
 * Also supports legacy CLI mode: ./rtl8188_cli scan|info|status|...
 */

#include <ncurses.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <ctype.h>

#define DEV_PATH  "/dev/rtl8188"
#define BUF_SIZE  (32 * 1024)
#define MAX_APS   64
#define MAX_CAP   128

/* ── colour pairs ─────────────────────────────────────────── */
enum {
	CP_NORMAL = 1,
	CP_HEADER,
	CP_MENU,
	CP_MENU_SEL,
	CP_STATUS_BAR,
	CP_TABLE_HDR,
	CP_CONNECTED,
	CP_SIGNAL_HI,
	CP_SIGNAL_MED,
	CP_SIGNAL_LO,
	CP_ERROR,
	CP_BAR_ARP,
	CP_BAR_IP4,
	CP_BAR_IP6,
	CP_BAR_OTH,
	CP_INPUT,
	CP_BAR_TCP,
	CP_BAR_UDP,
	CP_BAR_ICMP,
	CP_BAR_CHAT,
};

/* ── tab ids ──────────────────────────────────────────────── */
enum {
	TAB_INFO = 0,
	TAB_SCAN,
	TAB_STATUS,
	TAB_MONITOR,
	TAB_CAPTURE,
	TAB_CONNECT,
	TAB_COUNT
};

static const char *tab_labels[] = {
	"F1 Info", "F2 Scan", "F3 Status",
	"F4 Monitor", "F5 Capture", "F6 Connect"
};

/* ── parsed AP entry ──────────────────────────────────────── */
struct ap_entry {
	char bssid[20];
	char ssid[64];
	int  channel;
	int  signal;       /* dBm, negative */
	int  associated;
};

/* forward declaration */
static const char *strnstr_wrap(const char *hay, const char *needle, int len);

/* ── global state ─────────────────────────────────────────── */
static int  g_tab = TAB_INFO;
static int  g_running = 1;
static char g_resp[BUF_SIZE];
static int  g_resp_len;

static struct ap_entry g_aps[MAX_APS];
static int  g_ap_count;
static int  g_scan_scroll;

static int  g_cap_scroll;   /* selected packet index in capture list */

static int  g_monitor_on;
static int  g_filter_port;  /* 0 = no filter */

/* ── Parsed capture packet ───────────────────────────────── */
#define MAX_CAP_PKTS   128
#define MAX_DET_LINES  10

struct cap_pkt {
	char hdr[128];              /* "[N] Xs | Proto | NB [CHAT-AES]" */
	char addr[80];              /* "IP:port->IP:port" */
	char detail[MAX_DET_LINES][128]; /* CF_VER, CF_TYPE, IV, HMAC, ENC... */
	int  ndetail;
	int  is_chat;
};

static struct cap_pkt g_caplist[MAX_CAP_PKTS];
static int g_caplist_count;

/* connect form state */
static char g_ssid[64];
static char g_pass[128];
static int  g_connect_field;  /* 0 = ssid, 1 = password */
static char g_connect_msg[256];

/* ================================================================
 * Device communication
 * ================================================================ */

static int dev_command(const char *cmd, char *out, int out_sz)
{
	int fd, fd2, total = 0;
	ssize_t n;

	fd = open(DEV_PATH, O_RDWR);
	if (fd < 0)
		return -1;

	write(fd, cmd, strlen(cmd));
	close(fd);

	fd2 = open(DEV_PATH, O_RDONLY);
	if (fd2 < 0)
		return -1;

	while (total < out_sz - 1) {
		n = read(fd2, out + total, out_sz - 1 - total);
		if (n <= 0)
			break;
		total += n;
	}
	out[total] = '\0';
	close(fd2);
	return total;
}

/* ================================================================
 * Scan result parser  (parses raw iw scan output)
 * ================================================================ */

static int freq_to_channel(int freq)
{
	if (freq == 2484) return 14;
	if (freq >= 2412 && freq <= 2472)
		return (freq - 2412) / 5 + 1;
	if (freq >= 5180 && freq <= 5825)
		return (freq - 5000) / 5;
	return 0;
}

static void parse_scan_results(const char *raw)
{
	const char *p = raw;
	const char *next;

	g_ap_count = 0;

	while ((p = strstr(p, "BSS ")) != NULL && g_ap_count < MAX_APS) {
		struct ap_entry *ap = &g_aps[g_ap_count];
		memset(ap, 0, sizeof(*ap));

		/* BSSID */
		if (sscanf(p + 4, "%17s", ap->bssid) != 1) {
			p++;
			continue;
		}

		/* check "-- associated" on same line */
		{
			const char *eol = strchr(p, '\n');
			if (eol && strnstr_wrap(p, "associated", eol - p))
				ap->associated = 1;
		}

		/* Find extent of this BSS block */
		next = strstr(p + 4, "\nBSS ");
		int section = next ? (int)(next - p) : (int)strlen(p);

		/* freq → channel */
		{
			const char *f = strnstr_wrap(p, "freq: ", section);
			if (f) ap->channel = freq_to_channel(atoi(f + 6));
		}

		/* signal */
		{
			const char *s = strnstr_wrap(p, "signal: ", section);
			if (s) ap->signal = atoi(s + 8);
		}

		/* SSID */
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
		p += (next ? (next - p) : section);
	}
}

/* bounded strstr helper */
static const char *strnstr_wrap(const char *hay, const char *needle, int len)
{
	int nlen = strlen(needle);
	int i;
	if (nlen > len) return NULL;
	for (i = 0; i <= len - nlen; i++)
		if (strncmp(hay + i, needle, nlen) == 0)
			return hay + i;
	return NULL;
}

/* ================================================================
 * Drawing helpers
 * ================================================================ */

static void draw_header(int cols)
{
	attron(COLOR_PAIR(CP_HEADER) | A_BOLD);
	mvhline(0, 0, ' ', cols);
	mvprintw(0, (cols - 38) / 2, "RTL8188ETV WiFi Companion Monitor v1.0");
	attroff(COLOR_PAIR(CP_HEADER) | A_BOLD);
}

static void draw_menu(int cols)
{
	int x = 0, i;

	attron(COLOR_PAIR(CP_MENU));
	mvhline(1, 0, ' ', cols);

	for (i = 0; i < TAB_COUNT; i++) {
		if (i == g_tab) {
			attroff(COLOR_PAIR(CP_MENU));
			attron(COLOR_PAIR(CP_MENU_SEL) | A_BOLD);
		}
		mvprintw(1, x + 1, " %s ", tab_labels[i]);
		if (i == g_tab) {
			attroff(COLOR_PAIR(CP_MENU_SEL) | A_BOLD);
			attron(COLOR_PAIR(CP_MENU));
		}
		x += strlen(tab_labels[i]) + 3;
	}

	/* quit hint on far right */
	mvprintw(1, cols - 12, " F10 Quit ");
	attroff(COLOR_PAIR(CP_MENU));
}

static void draw_status_bar(int rows, int cols)
{
	char status_text[256];
	int fd, n = 0;
	char buf[512];

	/* Quick status probe */
	fd = open(DEV_PATH, O_RDWR);
	if (fd >= 0) {
		write(fd, "info", 4);
		close(fd);
		fd = open(DEV_PATH, O_RDONLY);
		if (fd >= 0) {
			n = read(fd, buf, sizeof(buf) - 1);
			if (n > 0) buf[n] = '\0';
			close(fd);
		}
	}

	/* Parse quick info for status bar */
	{
		const char *iface = strstr(buf, "Interface:");
		const char *mac   = strstr(buf, "MAC Address:");
		char ifname[32] = "---", macaddr[24] = "---";

		if (iface) sscanf(iface + 10, "%31s", ifname);
		if (mac)   sscanf(mac + 12, "%23s", macaddr);

		snprintf(status_text, sizeof(status_text),
			 " Dev: %s | MAC: %s | Module: /dev/rtl8188 ",
			 ifname, macaddr);
	}

	attron(COLOR_PAIR(CP_STATUS_BAR) | A_BOLD);
	mvhline(rows - 1, 0, ' ', cols);
	mvprintw(rows - 1, 0, "%.*s", cols, status_text);
	attroff(COLOR_PAIR(CP_STATUS_BAR) | A_BOLD);
}

static void draw_box(int y, int x, int h, int w, const char *title)
{
	int i;

	mvaddch(y, x, ACS_ULCORNER);
	mvaddch(y, x + w - 1, ACS_URCORNER);
	mvaddch(y + h - 1, x, ACS_LLCORNER);
	mvaddch(y + h - 1, x + w - 1, ACS_LRCORNER);
	for (i = 1; i < w - 1; i++) {
		mvaddch(y, x + i, ACS_HLINE);
		mvaddch(y + h - 1, x + i, ACS_HLINE);
	}
	for (i = 1; i < h - 1; i++) {
		mvaddch(y + i, x, ACS_VLINE);
		mvaddch(y + i, x + w - 1, ACS_VLINE);
	}
	if (title) {
		attron(A_BOLD | COLOR_PAIR(CP_HEADER));
		mvprintw(y, x + 2, " %s ", title);
		attroff(A_BOLD | COLOR_PAIR(CP_HEADER));
	}
}

static int tui_prompt_port(int rows, int cols)
{
	char buf[8];
	int bx = (cols - 44) / 2;
	int by = rows / 2 - 2;
	int field_y, field_x, n;

	if (bx < 1) bx = 1;

	draw_box(by, bx, 5, 44, "Filter by Port");

	attron(COLOR_PAIR(CP_NORMAL));
	mvprintw(by + 1, bx + 2, "Port (0=clear, 9090=chat): ");
	mvprintw(by + 3, bx + 2, "[Enter] OK   [Esc] Cancel");

	field_y = by + 1;
	field_x = bx + 29;

	attron(COLOR_PAIR(CP_INPUT) | A_UNDERLINE);
	mvprintw(field_y, field_x, "          ");
	move(field_y, field_x);
	attroff(A_UNDERLINE);

	curs_set(1);
	echo();
	keypad(stdscr, FALSE);
	timeout(-1);
	refresh();

	memset(buf, 0, sizeof(buf));
	n = mvgetnstr(field_y, field_x, buf, 5);

	noecho();
	keypad(stdscr, TRUE);
	curs_set(0);
	timeout(500);

	if (n == ERR)
		return -1;

	buf[5] = '\0';

	if (buf[0] == '\0' || buf[0] == 27)
		return -1;

	return atoi(buf);
}

/* ================================================================
 * Tab: Info (F1)
 * ================================================================ */

static void draw_tab_info(int top, int bot, int cols)
{
	g_resp_len = dev_command("info", g_resp, BUF_SIZE);
	if (g_resp_len <= 0) {
		mvprintw(top + 2, 3, "Cannot communicate with /dev/rtl8188");
		return;
	}

	/* Print the formatted info response line by line */
	int y = top + 1;
	char *line = strtok(g_resp, "\n");

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
 * Tab: Scan (F2)   — with parsed table
 * ================================================================ */

static void draw_tab_scan(int top, int bot, int cols)
{
	int y, i, visible;

	if (g_ap_count == 0) {
		attron(COLOR_PAIR(CP_TABLE_HDR));
		mvprintw(top + 2, 3,
			 "%-*s", cols - 6,
			 "Press 's' to scan for WiFi networks");
		attroff(COLOR_PAIR(CP_TABLE_HDR));
		return;
	}

	/* Table header */
	attron(COLOR_PAIR(CP_TABLE_HDR) | A_BOLD);
	mvprintw(top + 1, 2,
		 "%-4s %-20s %-18s %-4s %-8s %s",
		 "No", "SSID", "BSSID", "CH", "SIGNAL", "");
	mvhline(top + 2, 2, ACS_HLINE, cols - 4);
	attroff(COLOR_PAIR(CP_TABLE_HDR) | A_BOLD);

	visible = bot - top - 4;
	if (g_scan_scroll > g_ap_count - visible && g_ap_count > visible)
		g_scan_scroll = g_ap_count - visible;
	if (g_scan_scroll < 0)
		g_scan_scroll = 0;

	y = top + 3;
	for (i = g_scan_scroll; i < g_ap_count && y < bot - 1; i++, y++) {
		struct ap_entry *ap = &g_aps[i];
		int pair;

		if (ap->associated) {
			pair = CP_CONNECTED;
		} else if (ap->signal >= -50) {
			pair = CP_SIGNAL_HI;
		} else if (ap->signal >= -70) {
			pair = CP_SIGNAL_MED;
		} else {
			pair = CP_SIGNAL_LO;
		}

		attron(COLOR_PAIR(pair));
		mvprintw(y, 2, "%-4d %-20.20s %-18s %-4d %4d dBm %s",
			 i + 1,
			 ap->ssid[0] ? ap->ssid : "(hidden)",
			 ap->bssid, ap->channel, ap->signal,
			 ap->associated ? " <<< connected" : "");
		attroff(COLOR_PAIR(pair));
	}

	/* scrollbar hint */
	if (g_ap_count > visible) {
		attron(COLOR_PAIR(CP_MENU));
		mvprintw(bot - 1, 2,
			 " [UP/DOWN to scroll | %d APs | showing %d-%d] ",
			 g_ap_count, g_scan_scroll + 1,
			 g_scan_scroll + visible < g_ap_count ?
			 g_scan_scroll + visible : g_ap_count);
		attroff(COLOR_PAIR(CP_MENU));
	}
}

/* ================================================================
 * Tab: Status (F3)
 * ================================================================ */

static void draw_tab_status(int top, int bot, int cols)
{
	int y;
	char *line;

	g_resp_len = dev_command("status", g_resp, BUF_SIZE);
	if (g_resp_len <= 0) {
		mvprintw(top + 2, 3, "No data");
		return;
	}

	y = top + 1;
	line = strtok(g_resp, "\n");
	while (line && y < bot) {
		if (strstr(line, "===")) {
			attron(COLOR_PAIR(CP_HEADER) | A_BOLD);
		} else if (strstr(line, "Connected to")) {
			attron(COLOR_PAIR(CP_CONNECTED) | A_BOLD);
		} else if (strstr(line, "Not connected")) {
			attron(COLOR_PAIR(CP_ERROR));
		} else if (strstr(line, "SSID:") || strstr(line, "signal:")) {
			attron(A_BOLD);
		}
		mvprintw(y, 2, "%-*.*s", cols - 4, cols - 4, line);
		attrset(COLOR_PAIR(CP_NORMAL));
		y++;
		line = strtok(NULL, "\n");
	}
}

/* ================================================================
 * Tab: Monitor (F4) — auto-refreshing stats + bar chart
 * ================================================================ */

static void draw_bar(int y, int x, int max_w, int value, int total, int cp)
{
	int w = total > 0 ? (value * max_w) / total : 0;
	int i;

	if (w < 1 && value > 0)
		w = 1;

	attron(COLOR_PAIR(cp));
	for (i = 0; i < w; i++)
		mvaddch(y, x + i, ACS_BLOCK);
	attroff(COLOR_PAIR(cp));
}

static void draw_tab_monitor(int top, int bot, int cols)
{
	int y;
	char *line;
	int arp = 0, ip4 = 0, ip6 = 0, oth = 0, total_l2;
	int tcp = 0, udp = 0, icmp = 0, chat = 0, total_l4;
	int filter_port_val = 0, filter_matched = 0;
	int bar_w;

	g_resp_len = dev_command("stats", g_resp, BUF_SIZE);
	if (g_resp_len <= 0) {
		mvprintw(top + 2, 3, "No data");
		return;
	}

	{
		const char *p;
		p = strstr(g_resp, "ARP:");
		if (p) arp = atoi(p + 4);
		p = strstr(g_resp, "IPv4:");
		if (p) ip4 = atoi(p + 5);
		p = strstr(g_resp, "IPv6:");
		if (p) ip6 = atoi(p + 5);
		p = strstr(g_resp, "Other:");
		if (p) oth = atoi(p + 6);
		p = strstr(g_resp, "TCP:");
		if (p) tcp = atoi(p + 4);
		p = strstr(g_resp, "UDP:");
		if (p) udp = atoi(p + 4);
		p = strstr(g_resp, "ICMP:");
		if (p) icmp = atoi(p + 5);
		p = strstr(g_resp, "Chat(9090):");
		if (p) chat = atoi(p + 11);
		/* parse dynamic filter section: "[Filter: port N]" */
		p = strstr(g_resp, "[Filter: port ");
		if (p) {
			filter_port_val = atoi(p + 14);
			p = strstr(p, "Matched pkts:");
			if (p) filter_matched = atoi(p + 13);
		}
	}
	total_l2 = arp + ip4 + ip6 + oth;
	total_l4 = tcp + udp + icmp;

	y = top + 1;
	char resp_copy[BUF_SIZE];
	strncpy(resp_copy, g_resp, BUF_SIZE - 1);
	resp_copy[BUF_SIZE - 1] = '\0';

	line = strtok(resp_copy, "\n");
	while (line && y < bot - 14) {
		if (strstr(line, "===="))
			attron(COLOR_PAIR(CP_HEADER) | A_BOLD);
		else if (strstr(line, "[Filter: port"))
			attron(COLOR_PAIR(CP_BAR_CHAT) | A_BOLD);
		else if (strstr(line, "Matched pkts:"))
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

	bar_w = cols - 30;
	if (bar_w < 10) bar_w = 10;

	if (total_l2 > 0) {
		y += 1;
		attron(COLOR_PAIR(CP_TABLE_HDR) | A_BOLD);
		mvprintw(y++, 2, "L2 Protocol Distribution:");
		attroff(A_BOLD);
		y++;

		mvprintw(y, 2, "  ARP  %6d ", arp);
		draw_bar(y, 17, bar_w, arp, total_l2, CP_BAR_ARP);
		y++;
		mvprintw(y, 2, "  IPv4 %6d ", ip4);
		draw_bar(y, 17, bar_w, ip4, total_l2, CP_BAR_IP4);
		y++;
		mvprintw(y, 2, "  IPv6 %6d ", ip6);
		draw_bar(y, 17, bar_w, ip6, total_l2, CP_BAR_IP6);
		y++;
		mvprintw(y, 2, "  Oth  %6d ", oth);
		draw_bar(y, 17, bar_w, oth, total_l2, CP_BAR_OTH);
		y++;
	}

	if (total_l4 > 0) {
		y += 1;
		attron(COLOR_PAIR(CP_TABLE_HDR) | A_BOLD);
		mvprintw(y++, 2, "L4 Protocol Distribution:");
		attroff(A_BOLD);
		y++;

		mvprintw(y, 2, "  TCP  %6d ", tcp);
		draw_bar(y, 17, bar_w, tcp, total_l4, CP_BAR_TCP);
		y++;
		mvprintw(y, 2, "  UDP  %6d ", udp);
		draw_bar(y, 17, bar_w, udp, total_l4, CP_BAR_UDP);
		y++;
		mvprintw(y, 2, "  ICMP %6d ", icmp);
		draw_bar(y, 17, bar_w, icmp, total_l4, CP_BAR_ICMP);
		y++;
	}

	/* Filter port bar — shows actual filtered port from kernel */
	if (filter_port_val > 0 && y < bot - 3) {
		int ref = total_l4 > 0 ? total_l4 : (filter_matched > 0 ? filter_matched : 1);
		y++;
		attron(COLOR_PAIR(CP_TABLE_HDR) | A_BOLD);
		mvprintw(y++, 2, "Port Filter:");
		attroff(A_BOLD);
		y++;
		mvprintw(y, 2, "  :%d %5d ", filter_port_val, filter_matched);
		draw_bar(y, 17, bar_w, filter_matched, ref, CP_BAR_CHAT);
		y++;
	} else if (chat > 0 && y < bot - 2) {
		y++;
		attron(COLOR_PAIR(CP_BAR_CHAT) | A_BOLD);
		mvprintw(y, 2, "  ** CryptoChat(9090): %d pkts **", chat);
		attroff(COLOR_PAIR(CP_BAR_CHAT) | A_BOLD);
	}

	attron(COLOR_PAIR(CP_MENU));
	if (g_filter_port > 0)
		mvprintw(bot - 1, 2,
			 " [2s refresh | 'm' monitor | 'f' filter (port %d) | 'F' clear | 'r' refresh] ",
			 g_filter_port);
	else
		mvprintw(bot - 1, 2,
			 " [2s refresh | 'm' monitor | 'f' set filter | 'r' refresh] ");
	attroff(COLOR_PAIR(CP_MENU));
}

/* ================================================================
 * Tab: Capture (F5) — split-pane: list + detail
 * ================================================================ */

static void parse_capture_output(const char *buf)
{
	static char tmp[BUF_SIZE];
	char *line, *save;
	int pkt = -1;
	int len;

	g_caplist_count = 0;
	/* Use g_resp_len to copy exact bytes — avoids truncation at embedded nulls */
	len = g_resp_len < BUF_SIZE - 1 ? g_resp_len : BUF_SIZE - 2;
	memcpy(tmp, buf, len);
	tmp[len] = '\0';

	line = strtok_r(tmp, "\n", &save);
	while (line) {
		/* New packet: line starts with '[' then digit */
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
				strncpy(g_caplist[pkt].addr, line + 5,
					sizeof(g_caplist[pkt].addr) - 1);
			} else if ((strncmp(line, "CF_", 3) == 0 ||
				    strncmp(line, "IV:", 3) == 0 ||
				    strncmp(line, "HMAC1:", 6) == 0 ||
				    strncmp(line, "HMAC2:", 6) == 0 ||
				    strncmp(line, "ENC1:", 5) == 0 ||
				    strncmp(line, "ENC2:", 5) == 0 ||
				    strncmp(line, "DATA:", 5) == 0) &&
				   nd < MAX_DET_LINES) {
				strncpy(g_caplist[pkt].detail[nd], line,
					sizeof(g_caplist[pkt].detail[nd]) - 1);
				g_caplist[pkt].ndetail++;
			}
		}
		line = strtok_r(NULL, "\n", &save);
	}
}

static void draw_capture_list(int top, int bot, int cols, int sel)
{
	int y = top;
	int visible = bot - top;
	int start = 0;
	int i;

	/* Scroll so selection stays visible */
	if (sel >= visible)
		start = sel - visible + 1;

	for (i = start; i < g_caplist_count && y < bot; i++) {
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
		y++;
	}

	if (g_caplist_count == 0) {
		attron(COLOR_PAIR(CP_SIGNAL_LO));
		mvprintw(top + 1, 3, "(no packets captured)");
		attroff(COLOR_PAIR(CP_SIGNAL_LO));
	}
}

static void draw_capture_detail(int top, int bot, int cols, int sel)
{
	struct cap_pkt *p;
	int y = top;
	int i;
	int w = cols - 6;

	if (w < 10) w = 10;

	if (g_caplist_count == 0 || sel >= g_caplist_count) {
		mvprintw(top, 3, "(select a packet above with UP/DOWN)");
		return;
	}

	p = &g_caplist[sel];

	/* Address */
	if (p->addr[0] && y < bot) {
		attron(COLOR_PAIR(CP_CONNECTED) | A_BOLD);
		mvprintw(y++, 3, "%-*.*s", w, w, p->addr);
		attroff(A_BOLD);
	}

	/* Detail lines with distinct colors per field */
	for (i = 0; i < p->ndetail && y < bot; i++) {
		const char *d = p->detail[i];
		int attr;

		if (strncmp(d, "CF_TYPE:", 8) == 0)
			attr = COLOR_PAIR(CP_SIGNAL_HI) | A_BOLD;
		else if (strncmp(d, "CF_", 3) == 0)
			attr = COLOR_PAIR(CP_TABLE_HDR);
		else if (strncmp(d, "IV:", 3) == 0)
			attr = COLOR_PAIR(CP_BAR_ICMP) | A_BOLD;
		else if (strncmp(d, "HMAC1:", 6) == 0 ||
			 strncmp(d, "HMAC2:", 6) == 0)
			attr = COLOR_PAIR(CP_BAR_UDP) | A_BOLD;
		else if (strncmp(d, "ENC1:", 5) == 0 ||
			 strncmp(d, "ENC2:", 5) == 0)
			attr = COLOR_PAIR(CP_BAR_CHAT) | A_BOLD;
		else
			attr = COLOR_PAIR(CP_NORMAL);

		attron(attr);
		mvprintw(y++, 3, "%-*.*s", w, w, d);
		attroff(attr);
	}

	if (p->ndetail == 0 && y < bot) {
		attron(COLOR_PAIR(CP_SIGNAL_LO));
		mvprintw(y, 3, "(no detail data for this packet)");
		attroff(COLOR_PAIR(CP_SIGNAL_LO));
	}
}

static void draw_tab_capture(int top, int bot, int cols)
{
	int split, sel;

	g_resp_len = dev_command("capture", g_resp, BUF_SIZE);
	if (g_resp_len > 0)
		parse_capture_output(g_resp);

	/* Clamp selection */
	sel = g_cap_scroll;
	if (g_caplist_count > 0 && sel >= g_caplist_count)
		sel = g_caplist_count - 1;
	if (sel < 0) sel = 0;
	g_cap_scroll = sel;

	/* Split: 55% list, 45% detail */
	split = top + (bot - top) * 55 / 100;

	/* ── Summary list ── */
	draw_capture_list(top + 1, split, cols, sel);

	/* ── Separator ── */
	attron(COLOR_PAIR(CP_TABLE_HDR) | A_BOLD);
	mvhline(split, 0, ACS_HLINE, cols);
	if (g_caplist_count > 0 && sel < g_caplist_count) {
		char title[48];
		snprintf(title, sizeof(title), "[ Packet %d Detail ]", sel);
		mvprintw(split, (cols - (int)strlen(title)) / 2, "%s", title);
	}
	attroff(COLOR_PAIR(CP_TABLE_HDR) | A_BOLD);

	/* ── Detail panel ── */
	draw_capture_detail(split + 1, bot - 1, cols, sel);

	/* ── Hint ── */
	attron(COLOR_PAIR(CP_MENU));
	if (g_filter_port > 0)
		mvprintw(bot - 1, 1,
			 " UP/DN:select | r:refresh | f:filter(%d) | F:clear"
			 " | IV=grn HMAC1/2=mag ENC1/2=red ",
			 g_filter_port);
	else
		mvprintw(bot - 1, 1,
			 " UP/DN:select | r:refresh | f:set filter"
			 " | IV=grn HMAC1/2=mag ENC1/2=red(ciphertext) ");
	attroff(COLOR_PAIR(CP_MENU));
}

/* ================================================================
 * Tab: Connect (F6) — input form
 * ================================================================ */

static void draw_tab_connect(int top, int bot, int cols)
{
	int mid = (cols - 50) / 2;

	if (mid < 2) mid = 2;

	draw_box(top + 2, mid, 10, 50, "Connect to WiFi");

	attron(COLOR_PAIR(CP_NORMAL));
	mvprintw(top + 4, mid + 3, "SSID:");
	mvprintw(top + 6, mid + 3, "Password:");
	mvprintw(top + 9, mid + 3, "[Enter] Connect  [Tab] Switch field");

	/* SSID field */
	if (g_connect_field == 0)
		attron(COLOR_PAIR(CP_INPUT) | A_UNDERLINE);
	else
		attron(COLOR_PAIR(CP_NORMAL));
	mvprintw(top + 4, mid + 14, "%-30.30s", g_ssid);
	attroff(A_UNDERLINE);

	/* Password field */
	if (g_connect_field == 1)
		attron(COLOR_PAIR(CP_INPUT) | A_UNDERLINE);
	else
		attron(COLOR_PAIR(CP_NORMAL));
	{
		char masked[128];
		int i;
		for (i = 0; i < (int)strlen(g_pass) && i < 30; i++)
			masked[i] = '*';
		masked[i] = '\0';
		mvprintw(top + 6, mid + 14, "%-30.30s", masked);
	}
	attroff(A_UNDERLINE);
	attrset(COLOR_PAIR(CP_NORMAL));

	/* Connection message */
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

static void handle_connect_input(int ch)
{
	char *field = g_connect_field == 0 ? g_ssid : g_pass;
	int maxlen  = g_connect_field == 0 ? 63 : 127;
	int len     = strlen(field);

	if (ch == '\t' || ch == KEY_DOWN || ch == KEY_UP) {
		g_connect_field = 1 - g_connect_field;
	} else if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
		if (len > 0) field[len - 1] = '\0';
	} else if (ch == '\n' || ch == KEY_ENTER) {
		if (g_ssid[0] == '\0') {
			snprintf(g_connect_msg, sizeof(g_connect_msg),
				 "ERROR: SSID is empty");
			return;
		}
		char cmd[256];
		if (g_pass[0])
			snprintf(cmd, sizeof(cmd),
				 "connect %s %s", g_ssid, g_pass);
		else
			snprintf(cmd, sizeof(cmd), "connect %s", g_ssid);

		snprintf(g_connect_msg, sizeof(g_connect_msg),
			 "Connecting to %s ...", g_ssid);

		/* draw the "connecting" message before blocking */
		refresh();

		g_resp_len = dev_command(cmd, g_resp, BUF_SIZE);
		if (g_resp_len > 0) {
			/* Copy first line of response */
			char *nl = strchr(g_resp, '\n');
			int clen = nl ? (int)(nl - g_resp) : g_resp_len;
			if (clen > 200) clen = 200;
			snprintf(g_connect_msg, sizeof(g_connect_msg),
				 "%.*s", clen, g_resp);
		} else {
			snprintf(g_connect_msg, sizeof(g_connect_msg),
				 "ERROR: No response from module");
		}
	} else if (isprint(ch) && len < maxlen) {
		field[len] = ch;
		field[len + 1] = '\0';
	}
}

/* ================================================================
 * Main TUI loop
 * ================================================================ */

static void init_colors(void)
{
	start_color();
	use_default_colors();

	init_pair(CP_NORMAL,     -1,            -1);
	init_pair(CP_HEADER,     COLOR_WHITE,   COLOR_BLUE);
	init_pair(CP_MENU,       COLOR_BLACK,   COLOR_CYAN);
	init_pair(CP_MENU_SEL,   COLOR_WHITE,   COLOR_BLUE);
	init_pair(CP_STATUS_BAR, COLOR_WHITE,   COLOR_BLUE);
	init_pair(CP_TABLE_HDR,  COLOR_CYAN,    -1);
	init_pair(CP_CONNECTED,  COLOR_GREEN,   -1);
	init_pair(CP_SIGNAL_HI,  COLOR_GREEN,   -1);
	init_pair(CP_SIGNAL_MED, COLOR_YELLOW,  -1);
	init_pair(CP_SIGNAL_LO,  COLOR_RED,     -1);
	init_pair(CP_ERROR,      COLOR_RED,     -1);
	init_pair(CP_BAR_ARP,    COLOR_BLACK,   COLOR_MAGENTA);
	init_pair(CP_BAR_IP4,    COLOR_BLACK,   COLOR_GREEN);
	init_pair(CP_BAR_IP6,    COLOR_BLACK,   COLOR_CYAN);
	init_pair(CP_BAR_OTH,    COLOR_BLACK,   COLOR_YELLOW);
	init_pair(CP_INPUT,      COLOR_WHITE,   COLOR_BLUE);
	init_pair(CP_BAR_TCP,    COLOR_BLACK,   COLOR_BLUE);
	init_pair(CP_BAR_UDP,    COLOR_WHITE,   COLOR_MAGENTA);
	init_pair(CP_BAR_ICMP,   COLOR_BLACK,   COLOR_WHITE);
	init_pair(CP_BAR_CHAT,   COLOR_WHITE,   COLOR_RED);
}

static void tui_main(void)
{
	int ch;
	int rows, cols;
	int content_top, content_bot;
	time_t last_refresh = 0;

	initscr();
	cbreak();
	noecho();
	keypad(stdscr, TRUE);
	curs_set(0);
	timeout(500);    /* 500ms getch timeout for auto-refresh */

	if (has_colors())
		init_colors();

	while (g_running) {
		getmaxyx(stdscr, rows, cols);
		erase();

		content_top = 2;
		content_bot = rows - 1;

		draw_header(cols);
		draw_menu(cols);

		/* Auto-refresh for monitor tab every 2s */
		if (g_tab == TAB_MONITOR || g_tab == TAB_CAPTURE) {
			time_t now = time(NULL);
			if (now - last_refresh >= 2) {
				last_refresh = now;
			}
		}

		switch (g_tab) {
		case TAB_INFO:    draw_tab_info(content_top, content_bot, cols); break;
		case TAB_SCAN:    draw_tab_scan(content_top, content_bot, cols); break;
		case TAB_STATUS:  draw_tab_status(content_top, content_bot, cols); break;
		case TAB_MONITOR: draw_tab_monitor(content_top, content_bot, cols); break;
		case TAB_CAPTURE: draw_tab_capture(content_top, content_bot, cols); break;
		case TAB_CONNECT: draw_tab_connect(content_top, content_bot, cols); break;
		}

		draw_status_bar(rows, cols);
		refresh();

		ch = getch();
		if (ch == ERR)
			continue;

		/* Global keys */
		switch (ch) {
		case KEY_F(1):  g_tab = TAB_INFO;    break;
		case KEY_F(2):  g_tab = TAB_SCAN;    break;
		case KEY_F(3):  g_tab = TAB_STATUS;  break;
		case KEY_F(4):  g_tab = TAB_MONITOR; break;
		case KEY_F(5):  g_tab = TAB_CAPTURE; break;
		case KEY_F(6):  g_tab = TAB_CONNECT; break;
		case KEY_F(10): g_running = 0;       break;
		case 'q':
		case 'Q':
			if (g_tab != TAB_CONNECT)
				g_running = 0;
			else
				handle_connect_input(ch);
			break;

		case 's':
		case 'S':
			if (g_tab == TAB_SCAN) {
				/* Clear content area and show scanning message */
				int sy;
				for (sy = content_top + 1; sy < content_bot; sy++)
					mvhline(sy, 0, ' ', cols);
				attron(COLOR_PAIR(CP_CONNECTED) | A_BOLD);
				mvprintw(content_top + 2, 3,
					 "Scanning for WiFi networks, please wait...");
				attroff(COLOR_PAIR(CP_CONNECTED) | A_BOLD);
				refresh();
				g_resp_len = dev_command("scan",
							 g_resp, BUF_SIZE);
				if (g_resp_len > 0)
					parse_scan_results(g_resp);
				g_scan_scroll = 0;
			}
			break;

		case 'r':
		case 'R':
			last_refresh = 0;
			break;

		case 'm':
		case 'M':
			if (g_tab == TAB_MONITOR) {
				if (g_monitor_on) {
					dev_command("monitor off", g_resp, BUF_SIZE);
					g_monitor_on = 0;
				} else {
					dev_command("monitor on", g_resp, BUF_SIZE);
					g_monitor_on = 1;
				}
			}
			break;

		case KEY_UP:
			if (g_tab == TAB_SCAN && g_scan_scroll > 0)
				g_scan_scroll--;
			else if (g_tab == TAB_CAPTURE && g_cap_scroll > 0)
				g_cap_scroll--;
			else if (g_tab == TAB_CONNECT)
				handle_connect_input(ch);
			break;

		case KEY_DOWN:
			if (g_tab == TAB_SCAN)
				g_scan_scroll++;
			else if (g_tab == TAB_CAPTURE)
				g_cap_scroll++;
			else if (g_tab == TAB_CONNECT)
				handle_connect_input(ch);
			break;

		case 'f':
			if (g_tab == TAB_MONITOR || g_tab == TAB_CAPTURE) {
				int port = tui_prompt_port(rows, cols);
				if (port >= 0 && port <= 65535) {
					char fcmd[32];
					snprintf(fcmd, sizeof(fcmd),
						 "filter %d", port);
					dev_command(fcmd, g_resp, BUF_SIZE);
					g_filter_port = port;
				}
			}
			break;

		case 'F':
			if (g_tab == TAB_MONITOR || g_tab == TAB_CAPTURE) {
				dev_command("filter 0", g_resp, BUF_SIZE);
				g_filter_port = 0;
			}
			break;

		case 'd':
		case 'D':
			if (g_tab == TAB_STATUS) {
				dev_command("disconnect", g_resp, BUF_SIZE);
			}
			break;

		default:
			if (g_tab == TAB_CONNECT)
				handle_connect_input(ch);
			break;
		}
	}

	endwin();
}

/* ================================================================
 * Legacy CLI mode (backward-compatible)
 * ================================================================ */

static int cli_main(int argc, char *argv[])
{
	char cmd[512];
	char buf[BUF_SIZE];
	int n;

	if (strcmp(argv[1], "help") == 0 ||
	    strcmp(argv[1], "--help") == 0 ||
	    strcmp(argv[1], "-h") == 0) {
		printf("RTL8188ETV WiFi Companion Monitor\n\n");
		printf("Usage: %s [command]\n", argv[0]);
		printf("  (no args)         Launch TUI dashboard\n");
		printf("  scan              Scan WiFi\n");
		printf("  connect SSID [pw] Connect\n");
		printf("  disconnect        Disconnect\n");
		printf("  status            Connection status\n");
		printf("  info              Device info\n");
		printf("  stats             Packet stats\n");
		printf("  monitor on|off    Toggle monitoring\n");
		printf("  capture           Captured packets\n");
		printf("  filter <port>     Filter by port\n");
		printf("  filter chat       Filter CryptoChat (9090)\n");
		printf("  filter 0          Remove filter\n");
		printf("  help              This help\n");
		return 0;
	}

	if (strcmp(argv[1], "scan") == 0) {
		strcpy(cmd, "scan");
	} else if (strcmp(argv[1], "connect") == 0) {
		if (argc < 3) {
			fprintf(stderr, "Usage: connect <ssid> [password]\n");
			return 1;
		}
		if (argc >= 4)
			snprintf(cmd, sizeof(cmd), "connect %s %s",
				 argv[2], argv[3]);
		else
			snprintf(cmd, sizeof(cmd), "connect %s", argv[2]);
	} else if (strcmp(argv[1], "disconnect") == 0) {
		strcpy(cmd, "disconnect");
	} else if (strcmp(argv[1], "status") == 0) {
		strcpy(cmd, "status");
	} else if (strcmp(argv[1], "info") == 0) {
		strcpy(cmd, "info");
	} else if (strcmp(argv[1], "stats") == 0) {
		strcpy(cmd, "stats");
	} else if (strcmp(argv[1], "monitor") == 0) {
		if (argc < 3) {
			fprintf(stderr, "Usage: monitor on|off\n");
			return 1;
		}
		snprintf(cmd, sizeof(cmd), "monitor %s", argv[2]);
	} else if (strcmp(argv[1], "capture") == 0) {
		strcpy(cmd, "capture");
	} else if (strcmp(argv[1], "filter") == 0) {
		if (argc < 3) {
			strcpy(cmd, "filter");
		} else {
			snprintf(cmd, sizeof(cmd), "filter %s", argv[2]);
		}
	} else {
		fprintf(stderr, "Unknown command: %s\n", argv[1]);
		return 1;
	}

	n = dev_command(cmd, buf, BUF_SIZE);
	if (n > 0) {
		printf("%s", buf);
		if (n > 0 && buf[n - 1] != '\n')
			printf("\n");
	} else {
		fprintf(stderr, "Error: cannot communicate with /dev/rtl8188\n");
		return 1;
	}
	return 0;
}

/* ================================================================ */

int main(int argc, char *argv[])
{
	if (argc >= 2)
		return cli_main(argc, argv);

	tui_main();
	return 0;
}
