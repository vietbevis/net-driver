/*
 * RTL8188ETV WiFi Companion Monitor — CLI Mode và main()
 *
 * Chạy không có tham số: mở TUI dashboard (tui_main).
 * Chạy với tham số: CLI truyền thống (cli_main).
 *
 * Ví dụ CLI:
 *   ./rtl8188_cli scan
 *   ./rtl8188_cli connect MyWiFi mypass
 *   ./rtl8188_cli monitor on
 *   ./rtl8188_cli capture
 */

#include "rtl8188_cli.h"

/**
 * cli_main() - Chế độ CLI: gửi lệnh và in kết quả ra stdout
 * @argc: Số tham số dòng lệnh
 * @argv: Mảng tham số
 *
 * Chuyển đổi tham số dòng lệnh thành chuỗi lệnh cho kernel module,
 * gọi dev_command() và in kết quả ra stdout.
 *
 * Trả về: 0 thành công, 1 nếu có lỗi.
 */
int cli_main(int argc, char *argv[])
{
	char cmd[512];
	char buf[BUF_SIZE];
	int n;

	/* Hiển thị help */
	if (strcmp(argv[1], "help")   == 0 ||
	    strcmp(argv[1], "--help") == 0 ||
	    strcmp(argv[1], "-h")     == 0) {
		printf("RTL8188ETV WiFi Companion Monitor\n\n");
		printf("Cách dùng: %s [lệnh]\n", argv[0]);
		printf("  (không có tham số)    Mở giao diện TUI\n");
		printf("  scan                  Quét WiFi\n");
		printf("  connect SSID [pass]   Kết nối mạng\n");
		printf("  disconnect            Ngắt kết nối\n");
		printf("  status                Trạng thái kết nối\n");
		printf("  info                  Thông tin thiết bị\n");
		printf("  stats                 Thống kê gói tin\n");
		printf("  monitor on|off        Bật/tắt packet monitoring\n");
		printf("  capture               Xem gói tin đã bắt\n");
		printf("  filter <port>         Lọc theo port\n");
		printf("  filter chat           Lọc CryptoChat (port 9090)\n");
		printf("  filter 0              Xóa filter\n");
		printf("  help                  Hiển thị trợ giúp này\n");
		return 0;
	}

	/* Map tham số CLI thành lệnh kernel */
	if (strcmp(argv[1], "scan") == 0) {
		strcpy(cmd, "scan");

	} else if (strcmp(argv[1], "connect") == 0) {
		if (argc < 3) {
			fprintf(stderr, "Cách dùng: connect <ssid> [mật_khẩu]\n");
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
			fprintf(stderr, "Cách dùng: monitor on|off\n");
			return 1;
		}
		snprintf(cmd, sizeof(cmd), "monitor %s", argv[2]);

	} else if (strcmp(argv[1], "capture") == 0) {
		strcpy(cmd, "capture");

	} else if (strcmp(argv[1], "filter") == 0) {
		if (argc < 3)
			strcpy(cmd, "filter");
		else
			snprintf(cmd, sizeof(cmd), "filter %s", argv[2]);

	} else {
		fprintf(stderr, "Lệnh không hợp lệ: %s\n"
			"Chạy '%s help' để xem danh sách lệnh.\n",
			argv[1], argv[0]);
		return 1;
	}

	/* Gửi lệnh và in kết quả */
	n = dev_command(cmd, buf, BUF_SIZE);
	if (n > 0) {
		printf("%s", buf);
		/* Đảm bảo luôn kết thúc bằng newline */
		if (buf[n - 1] != '\n')
			printf("\n");
	} else {
		fprintf(stderr,
			"Lỗi: Không thể giao tiếp với /dev/rtl8188\n"
			"Hãy đảm bảo module đã được load: sudo make load\n");
		return 1;
	}

	return 0;
}

/**
 * main() - Entry point của chương trình
 *
 * Phân biệt hai chế độ dựa trên số tham số:
 *   argc == 1: Mở TUI dashboard (không tham số)
 *   argc >= 2: CLI mode với lệnh cụ thể
 */
int main(int argc, char *argv[])
{
	if (argc >= 2)
		return cli_main(argc, argv);

	tui_main();
	return 0;
}