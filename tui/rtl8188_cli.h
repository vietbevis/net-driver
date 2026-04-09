/*
 * RTL8188ETV WiFi Companion Monitor — TUI Header dùng chung
 *
 * Định nghĩa hằng số, kiểu dữ liệu, color pair, tab ID và
 * khai báo các hàm dùng chung giữa các file userspace.
 */

#ifndef RTL8188_CLI_H
#define RTL8188_CLI_H

#include <ncurses.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <ctype.h>

/* Đường dẫn đến character device của kernel module */
#define DEV_PATH  "/dev/rtl8188"

/* Kích thước buffer đọc phản hồi từ kernel module */
#define BUF_SIZE  (32 * 1024)

/* Số lượng AP tối đa từ kết quả scan */
#define MAX_APS   64

/* Số entry tối đa trong danh sách capture hiển thị */
#define MAX_CAP_PKTS  128

/* Số dòng detail tối đa cho một gói tin */
#define MAX_DET_LINES 10

/* ================================================================
 * Color pair IDs (khởi tạo trong tui_init_colors)
 * ================================================================ */
enum {
	CP_NORMAL     = 1,  /* Màu mặc định terminal */
	CP_HEADER,          /* Trắng trên nền xanh dương — title bar */
	CP_MENU,            /* Đen trên nền cyan — tab bar */
	CP_MENU_SEL,        /* Trắng trên nền xanh dương — tab đang chọn */
	CP_STATUS_BAR,      /* Trắng trên nền xanh dương — status bar dưới */
	CP_TABLE_HDR,       /* Cyan trên nền mặc định — tiêu đề bảng */
	CP_CONNECTED,       /* Xanh lá — đã kết nối / tín hiệu mạnh */
	CP_SIGNAL_HI,       /* Xanh lá — signal >= -50 dBm */
	CP_SIGNAL_MED,      /* Vàng — signal >= -70 dBm */
	CP_SIGNAL_LO,       /* Đỏ — signal < -70 dBm */
	CP_ERROR,           /* Đỏ — lỗi, cảnh báo */
	CP_BAR_ARP,         /* Đen trên nền tím — bar ARP */
	CP_BAR_IP4,         /* Đen trên nền xanh lá — bar IPv4 */
	CP_BAR_IP6,         /* Đen trên nền cyan — bar IPv6 */
	CP_BAR_OTH,         /* Đen trên nền vàng — bar Other */
	CP_INPUT,           /* Trắng trên nền xanh dương — input field */
	CP_BAR_TCP,         /* Đen trên nền xanh dương — bar TCP */
	CP_BAR_UDP,         /* Trắng trên nền tím — bar UDP */
	CP_BAR_ICMP,        /* Đen trên nền trắng — bar ICMP */
	CP_BAR_CHAT,        /* Trắng trên nền đỏ — bar CryptoChat */
};

/* ================================================================
 * Tab IDs
 * ================================================================ */
enum {
	TAB_INFO    = 0,  /* F1: Thông tin USB device */
	TAB_SCAN,         /* F2: Quét WiFi */
	TAB_STATUS,       /* F3: Trạng thái kết nối */
	TAB_MONITOR,      /* F4: Thống kê gói tin real-time */
	TAB_CAPTURE,      /* F5: Deep packet capture */
	TAB_CONNECT,      /* F6: Form kết nối WiFi */
	TAB_COUNT
};

/* Nhãn hiển thị trên tab bar */
extern const char *tab_labels[TAB_COUNT];

/* ================================================================
 * Cấu trúc một điểm truy cập WiFi (từ kết quả iw scan)
 * ================================================================ */
struct ap_entry {
	char bssid[20];     /* Địa chỉ MAC của AP, vd: "aa:bb:cc:dd:ee:ff" */
	char ssid[64];      /* Tên mạng (SSID), empty nếu hidden */
	int  channel;       /* Kênh WiFi (1-14 cho 2.4GHz) */
	int  signal;        /* Cường độ tín hiệu dBm (âm, vd: -65) */
	int  associated;    /* 1 nếu đây là AP đang kết nối */
};

/* ================================================================
 * Cấu trúc một gói tin đã phân tích (cho tab Capture)
 * ================================================================ */
struct cap_pkt {
	/* Dòng header: "[N] Xs | Proto | NB [CHAT-AES]" */
	char hdr[128];

	/* Địa chỉ nguồn/đích: "IP:port->IP:port" */
	char addr[80];

	/* Các dòng detail: CF_VER, CF_TYPE, IV, HMAC, ENC, DATA */
	char detail[MAX_DET_LINES][128];
	int  ndetail;

	/* 1 nếu gói này là CryptoChat (có [CHAT-AES] trong header) */
	int  is_chat;
};

/* ================================================================
 * Trạng thái toàn cục của TUI (định nghĩa trong rtl8188_tui.c)
 * ================================================================ */

/* Tab đang hiển thị */
extern int g_tab;

/* Flag vòng lặp chính: 0 = thoát */
extern int g_running;

/* Buffer nhận phản hồi từ kernel module */
extern char g_resp[BUF_SIZE];
extern int  g_resp_len;

/* Danh sách AP từ kết quả scan */
extern struct ap_entry g_aps[MAX_APS];
extern int  g_ap_count;
extern int  g_scan_scroll;   /* Vị trí cuộn trong danh sách AP */
extern char g_scan_msg[256]; /* Thông báo sau lần scan gần nhất */

/* Vị trí con trỏ trong danh sách gói capture */
extern int  g_cap_scroll;
extern int  g_cap_paused;  /* 1 = pause auto-refresh list to select stable */

/* Trạng thái monitoring và filter port */
extern int  g_monitor_on;
extern int  g_filter_port;   /* 0 = không filter */

/* Danh sách gói capture đã phân tích */
extern struct cap_pkt g_caplist[MAX_CAP_PKTS];
extern int g_caplist_count;

/* Dữ liệu form kết nối (tab Connect) */
extern char g_ssid[64];
extern char g_pass[128];
extern int  g_connect_field;   /* 0 = SSID field, 1 = Password field */
extern char g_connect_msg[256];

/* ================================================================
 * Khai báo hàm dùng chung
 * ================================================================ */

/* Giao tiếp với kernel module */
int dev_command(const char *cmd, char *out, int out_sz);

/* Khởi tạo màu sắc ncurses */
void tui_init_colors(void);

/* Vẽ các thành phần giao diện chung */
void draw_header(int cols);
void draw_menu(int cols);
void draw_status_bar(int rows, int cols);
void draw_box(int y, int x, int h, int w, const char *title);
void draw_bar(int y, int x, int max_w, int value, int total, int cp);

/* Dialog nhập port filter */
int tui_prompt_port(int rows, int cols);

/* Vẽ từng tab */
void draw_tab_info(int top, int bot, int cols);
void draw_tab_scan(int top, int bot, int cols);
void draw_tab_status(int top, int bot, int cols);
void draw_tab_monitor(int top, int bot, int cols);
void draw_tab_capture(int top, int bot, int cols);
void draw_tab_connect(int top, int bot, int cols);

/* Xử lý phím trong tab Connect */
void handle_connect_input(int ch);
void connect_submit_and_wait(int top, int bot, int cols);

/* Parse kết quả iw scan thành mảng g_aps[] */
void parse_scan_results(const char *raw);

/* Parse output lệnh capture thành g_caplist[] */
void parse_capture_output(const char *buf);

/* Entry point của giao diện TUI */
void tui_main(void);

/* CLI mode truyền thống */
int cli_main(int argc, char *argv[]);

#endif /* RTL8188_CLI_H */