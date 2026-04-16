/*
 * RTL8188ETV WiFi Companion Monitor — TUI Core
 *
 * Chứa:
 *   - Biến toàn cục của TUI
 *   - dev_command(): giao tiếp với /dev/rtl8188
 *   - tui_init_colors(): khởi tạo color pairs ncurses
 *   - Hàm vẽ chung: header, menu, status bar, box, bar chart
 *   - tui_prompt_port(): dialog nhập port filter
 *   - tui_main(): vòng lặp chính TUI
 */

#include "rtl8188_cli.h"
#include <locale.h>

/* ================================================================
 * Nhãn tab bar
 * ================================================================ */

const char *tab_labels[TAB_COUNT] = {
	"F1 Info", "F2 Scan", "F3 Status",
	"F4 Monitor", "F5 Capture", "F6 Connect"
};

/* ================================================================
 * Biến toàn cục TUI
 * ================================================================ */

int  g_tab       = TAB_INFO;  /* Tab đang hiển thị */
int  g_running   = 1;         /* Flag thoát vòng lặp */
char g_resp[BUF_SIZE];        /* Buffer phản hồi từ kernel */
int  g_resp_len;

struct ap_entry g_aps[MAX_APS]; /* Danh sách AP từ kết quả scan */
int  g_ap_count   = 0;
int  g_scan_scroll = 0;         /* Vị trí cuộn danh sách AP */
char g_scan_msg[256] = {0};     /* Thông báo sau lần scan gần nhất */

int  g_cap_scroll  = 0;         /* Con trỏ chọn gói trong capture list */
int  g_cap_paused  = 0;         /* Pause list updates in F5 */
int  g_monitor_on  = 0;         /* 1 nếu monitor đang bật */
int  g_filter_port = 0;         /* Cổng đang filter, 0 = tất cả */

struct cap_pkt g_caplist[MAX_CAP_PKTS]; /* Danh sách gói đã phân tích */
int g_caplist_count = 0;

/* Dữ liệu form kết nối */
char g_ssid[64]         = {0};
char g_pass[128]        = {0};
int  g_connect_field    = 0;  /* 0 = SSID, 1 = Password */
char g_connect_msg[256] = {0};

/* ================================================================
 * Giao tiếp với kernel module
 * ================================================================ */

/**
 * dev_command() - Gửi lệnh và nhận kết quả từ /dev/rtl8188
 * @cmd:    Chuỗi lệnh (vd: "scan", "info", "monitor on")
 * @out:    Buffer nhận kết quả
 * @out_sz: Kích thước buffer out
 *
 * Mở device hai lần (write + read) để tránh vấn đề về file position.
 * Kernel module reset f_pos về 0 sau mỗi write(), nên lần read() tiếp
 * theo trên fd mới sẽ đọc từ đầu resp_buf.
 *
 * Trả về: số byte đọc được, hoặc -1 nếu lỗi mở device.
 */
int dev_command(const char *cmd, char *out, int out_sz)
{
	int fd, fd2, total = 0;
	ssize_t n;

	/* Ghi lệnh */
	fd = open(DEV_PATH, O_RDWR);
	if (fd < 0)
		return -1;
	write(fd, cmd, strlen(cmd));
	close(fd);

	/* Đọc kết quả — mở lại để có file position = 0 */
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
 * Khởi tạo màu sắc ncurses
 * ================================================================ */

/**
 * tui_init_colors() - Khởi tạo 20 color pair cho giao diện TUI
 *
 * Dùng use_default_colors() với -1 để giữ màu nền terminal mặc định
 * thay vì ép màu đen, giúp giao diện trong suốt hơn trên terminal tối.
 */
void tui_init_colors(void)
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

/* ================================================================
 * Vẽ các thành phần giao diện chung
 * ================================================================ */

/**
 * draw_header() - Vẽ thanh tiêu đề ở dòng 0
 * @cols: Số cột terminal hiện tại
 */
void draw_header(int cols)
{
	attron(COLOR_PAIR(CP_HEADER) | A_BOLD);
	mvhline(0, 0, ' ', cols);
	mvprintw(0, (cols - 38) / 2, "RTL8188ETV WiFi Companion Monitor v1.0");
	attroff(COLOR_PAIR(CP_HEADER) | A_BOLD);
}

/**
 * draw_menu() - Vẽ thanh tab ở dòng 1
 * @cols: Số cột terminal hiện tại
 *
 * Tab đang chọn (g_tab) được highlight màu khác.
 * Hiển thị gợi ý "F10 Quit" ở góc phải.
 */
void draw_menu(int cols)
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

	mvprintw(1, cols - 12, " F10 Quit ");
	attroff(COLOR_PAIR(CP_MENU));
}

/**
 * draw_status_bar() - Vẽ thanh trạng thái ở dòng cuối
 * @rows: Số hàng terminal
 * @cols: Số cột terminal
 *
 * Hiển thị tên interface và địa chỉ MAC từ lệnh "info".
 * Gọi dev_command("info") mỗi lần vẽ — có thể tối ưu với cache nếu cần.
 */
void draw_status_bar(int rows, int cols)
{
	char status_text[256];
	int fd, n = 0;
	char buf[512];

	/* Lấy thông tin nhanh từ lệnh info */
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

	/* Trích tên interface và MAC từ output của lệnh info */
	{
		const char *iface = strstr(buf, "Interface:");
		const char *mac   = strstr(buf, "MAC Address:");
		char ifname[32]  = "---";
		char macaddr[24] = "---";

		if (iface) sscanf(iface + 10, "%31s", ifname);
		if (mac)   sscanf(mac   + 12, "%23s", macaddr);

		snprintf(status_text, sizeof(status_text),
			 " Dev: %s | MAC: %s | /dev/rtl8188 ",
			 ifname, macaddr);
	}

	attron(COLOR_PAIR(CP_STATUS_BAR) | A_BOLD);
	mvhline(rows - 1, 0, ' ', cols);
	mvprintw(rows - 1, 0, "%.*s", cols, status_text);
	attroff(COLOR_PAIR(CP_STATUS_BAR) | A_BOLD);
}

/**
 * draw_box() - Vẽ khung hộp (box) với tiêu đề tùy chọn
 * @y, @x: Góc trên trái
 * @h, @w: Chiều cao và chiều rộng
 * @title: Tiêu đề hiển thị trên cạnh trên (NULL = không có)
 */
void draw_box(int y, int x, int h, int w, const char *title)
{
	int i;

	mvaddch(y,         x,         ACS_ULCORNER);
	mvaddch(y,         x + w - 1, ACS_URCORNER);
	mvaddch(y + h - 1, x,         ACS_LLCORNER);
	mvaddch(y + h - 1, x + w - 1, ACS_LRCORNER);

	for (i = 1; i < w - 1; i++) {
		mvaddch(y,         x + i, ACS_HLINE);
		mvaddch(y + h - 1, x + i, ACS_HLINE);
	}
	for (i = 1; i < h - 1; i++) {
		mvaddch(y + i, x,         ACS_VLINE);
		mvaddch(y + i, x + w - 1, ACS_VLINE);
	}

	if (title) {
		attron(A_BOLD | COLOR_PAIR(CP_HEADER));
		mvprintw(y, x + 2, " %s ", title);
		attroff(A_BOLD | COLOR_PAIR(CP_HEADER));
	}
}

/**
 * draw_bar() - Vẽ một thanh ngang trong bar chart
 * @y, @x:   Vị trí bắt đầu vẽ
 * @max_w:   Độ rộng tối đa của thanh (pixel)
 * @value:   Giá trị của thanh này
 * @total:   Tổng giá trị (dùng tính tỷ lệ)
 * @cp:      Color pair để tô màu thanh
 *
 * Vẽ bằng ký tự ACS_BLOCK để trông như thanh đặc.
 */
void draw_bar(int y, int x, int max_w, int value, int total, int cp)
{
	int w = total > 0 ? (value * max_w) / total : 0;
	int i;

	/* Đảm bảo thanh luôn hiện ít nhất 1 ô nếu value > 0 */
	if (w < 1 && value > 0)
		w = 1;

	attron(COLOR_PAIR(cp));
	for (i = 0; i < w; i++)
		mvaddch(y, x + i, ACS_BLOCK);
	attroff(COLOR_PAIR(cp));
}

/**
 * tui_prompt_port() - Dialog nhập port number để filter
 * @rows, @cols: Kích thước terminal
 *
 * Hiển thị hộp dialog giữa màn hình, cho phép nhập số cổng 0-65535.
 * 0 = xóa filter, 9090 = CryptoChat.
 *
 * Trả về: port number (0-65535), hoặc -1 nếu hủy (Esc/Enter rỗng).
 */
int tui_prompt_port(int rows, int cols)
{
	char buf[8];
	int bx = (cols - 44) / 2;
	int by = rows / 2 - 2;
	int n;

	if (bx < 1) bx = 1;

	draw_box(by, bx, 5, 44, "Filter by Port");

	attron(COLOR_PAIR(CP_NORMAL));
	mvprintw(by + 1, bx + 2, "Port (0=clear, 9090=chat): ");
	mvprintw(by + 3, bx + 2, "[Enter] OK   [Esc] Cancel");

	attron(COLOR_PAIR(CP_INPUT) | A_UNDERLINE);
	mvprintw(by + 1, bx + 29, "          ");
	move(by + 1, bx + 29);
	attroff(A_UNDERLINE);

	/* Tạm thời bật cursor và echo để người dùng thấy gõ */
	curs_set(1);
	echo();
	keypad(stdscr, FALSE);
	timeout(-1);
	refresh();

	memset(buf, 0, sizeof(buf));
	n = mvgetnstr(by + 1, bx + 29, buf, 5);

	/* Khôi phục chế độ TUI */
	noecho();
	keypad(stdscr, TRUE);
	curs_set(0);
	timeout(500);

	if (n == ERR || buf[0] == '\0' || buf[0] == 27)
		return -1;

	return atoi(buf);
}

/* ================================================================
 * Vòng lặp chính TUI
 * ================================================================ */

/**
 * tui_main() - Khởi tạo ncurses và chạy vòng lặp sự kiện
 *
 * Vòng lặp:
 *   1. Lấy kích thước terminal (getmaxyx)
 *   2. Xóa màn hình (erase)
 *   3. Vẽ header, menu bar, nội dung tab hiện tại, status bar
 *   4. Refresh màn hình
 *   5. Đọc phím (timeout 500ms)
 *   6. Xử lý phím: chuyển tab, lệnh, cuộn
 *
 * Timeout 500ms của getch() tạo hiệu ứng auto-refresh 2 giây
 * cho các tab Monitor và Capture (kiểm tra last_refresh).
 */
void tui_main(void)
{
	int ch;
	int rows, cols;
	int content_top, content_bot;
	time_t last_refresh = 0;

	/* Enable UTF-8 / Vietnamese rendering if terminal supports it */
	setlocale(LC_ALL, "");

	initscr();
	cbreak();
	noecho();
	keypad(stdscr, TRUE);
	curs_set(0);
	timeout(500);  /* getch() không block quá 500ms */

	if (has_colors())
		tui_init_colors();

	while (g_running) {
		getmaxyx(stdscr, rows, cols);
		erase();

		/* Vùng nội dung: giữa header (row 1) và status bar (row cuối) */
		content_top = 2;
		content_bot = rows - 1;

		draw_header(cols);
		draw_menu(cols);

		/* Auto-refresh 2 giây cho tab live */
		if (g_tab == TAB_MONITOR || g_tab == TAB_CAPTURE) {
			time_t now = time(NULL);
			if (now - last_refresh >= 2)
				last_refresh = now;
		}

		/* Vẽ nội dung tab đang chọn */
		switch (g_tab) {
		case TAB_INFO:    draw_tab_info(content_top, content_bot, cols);    break;
		case TAB_SCAN:    draw_tab_scan(content_top, content_bot, cols);    break;
		case TAB_STATUS:  draw_tab_status(content_top, content_bot, cols);  break;
		case TAB_MONITOR: draw_tab_monitor(content_top, content_bot, cols); break;
		case TAB_CAPTURE: draw_tab_capture(content_top, content_bot, cols); break;
		case TAB_CONNECT: draw_tab_connect(content_top, content_bot, cols); break;
		}

		draw_status_bar(rows, cols);
		refresh();

		ch = getch();
		if (ch == ERR)
			continue;

		/* ---- Xử lý phím toàn cục ---- */
		switch (ch) {
		case KEY_F(1):  g_tab = TAB_INFO;    break;
		case KEY_F(2):  g_tab = TAB_SCAN;    break;
		case KEY_F(3):  g_tab = TAB_STATUS;  break;
		case KEY_F(4):  g_tab = TAB_MONITOR; break;
		case KEY_F(5):  g_tab = TAB_CAPTURE; break;
		case KEY_F(6):  g_tab = TAB_CONNECT; break;
		case KEY_F(10): g_running = 0;       break;

		case 'q': case 'Q':
			if (g_tab != TAB_CONNECT)
				g_running = 0;
			else
				handle_connect_input(ch);
			break;

		case 's': case 'S':
			if (g_tab == TAB_CONNECT) {
				handle_connect_input(ch);
			} else if (g_tab == TAB_SCAN) {
				/* Hiển thị thông báo "đang quét" trước khi block */
				int sy;
				for (sy = content_top + 1; sy < content_bot; sy++)
					mvhline(sy, 0, ' ', cols);
				attron(COLOR_PAIR(CP_CONNECTED) | A_BOLD);
				mvprintw(content_top + 2, 3,
					 "Đang quét WiFi, vui lòng chờ...");
				attroff(COLOR_PAIR(CP_CONNECTED) | A_BOLD);
				refresh();
				/* Block đến khi scan hoàn tất (có thể 5-10s) */
				g_resp_len = dev_command("scan", g_resp, BUF_SIZE);
				/* Lưu lại thông báo 1 dòng để UI không bị nháy về hint */
				g_scan_msg[0] = '\0';
				if (g_resp_len > 0) {
					char *nl = strchr(g_resp, '\n');
					int clen = nl ? (int)(nl - g_resp) : g_resp_len;
					if (clen > (int)sizeof(g_scan_msg) - 1)
						clen = (int)sizeof(g_scan_msg) - 1;
					snprintf(g_scan_msg, sizeof(g_scan_msg),
						 "%.*s", clen, g_resp);
					parse_scan_results(g_resp);
					if (g_ap_count == 0 && g_scan_msg[0] == '\0')
						snprintf(g_scan_msg, sizeof(g_scan_msg),
							 "Không tìm thấy AP nào (hoặc scan bị chặn).");
				} else {
					snprintf(g_scan_msg, sizeof(g_scan_msg),
						 "Scan không có phản hồi từ module.");
				}
				g_scan_scroll = 0;
			}
			break;

		case 'r': case 'R':
			if (g_tab == TAB_CONNECT)
				handle_connect_input(ch);
			else {
				/* In capture tab, treat refresh as "resume + refresh now" */
				if (g_tab == TAB_CAPTURE)
					g_cap_paused = 0;
				last_refresh = 0;  /* Force refresh ngay lần tiếp */
			}
			break;

		case 'p': case 'P':
			if (g_tab == TAB_CAPTURE)
				g_cap_paused = !g_cap_paused;
			else if (g_tab == TAB_CONNECT)
				handle_connect_input(ch);
			break;

		case 'm': case 'M':
			if (g_tab == TAB_CONNECT) {
				handle_connect_input(ch);
			} else if (g_tab == TAB_MONITOR) {
				/* Toggle packet monitoring */
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
			else if (g_tab == TAB_CAPTURE && g_cap_scroll > 0) {
				g_cap_scroll--;
				g_cap_paused = 1;
			} else if (g_tab == TAB_CONNECT) {
				handle_connect_input(ch);
			}
			break;

		case KEY_DOWN:
			if (g_tab == TAB_SCAN)
				g_scan_scroll++;
			else if (g_tab == TAB_CAPTURE) {
				g_cap_scroll++;
				g_cap_paused = 1;
			} else if (g_tab == TAB_CONNECT) {
				handle_connect_input(ch);
			}
			break;

		case 'f':
			if (g_tab == TAB_CONNECT) {
				handle_connect_input(ch);
			} else if (g_tab == TAB_MONITOR || g_tab == TAB_CAPTURE) {
				/* Mở dialog nhập port */
				int port = tui_prompt_port(rows, cols);
				if (port >= 0 && port <= 65535) {
					char fcmd[32];
					snprintf(fcmd, sizeof(fcmd), "filter %d", port);
					dev_command(fcmd, g_resp, BUF_SIZE);
					g_filter_port = port;
				}
			}
			break;

		case 'F':
			if (g_tab == TAB_CONNECT) {
				handle_connect_input(ch);
			} else if (g_tab == TAB_MONITOR || g_tab == TAB_CAPTURE) {
				/* Xóa filter */
				dev_command("filter 0", g_resp, BUF_SIZE);
				g_filter_port = 0;
			}
			break;

		case 'd': case 'D':
			if (g_tab == TAB_CONNECT)
				handle_connect_input(ch);
			else if (g_tab == TAB_STATUS)
				dev_command("disconnect", g_resp, BUF_SIZE);
			break;

		default:
			if (g_tab == TAB_CONNECT) {
				if (ch == '\n' || ch == KEY_ENTER)
					connect_submit_and_wait(content_top, content_bot, cols);
				else
					handle_connect_input(ch);
			}
			break;
		}
	}

	endwin();
}