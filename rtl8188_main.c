// SPDX-License-Identifier: GPL-2.0
/*
 * RTL8188ETV WiFi Companion Monitor — Module chính
 *
 * Chứa hàm init/exit của module, biến toàn cục g_mon,
 * biến môi trường helper_envp, và các hàm tiện ích dùng chung
 * (run_cmd, read_tmpfile, sanitize, set_resp).
 *
 * Luồng khởi tạo:
 *   rtl8188_mon_init()
 *     → cấp phát g_mon và resp_buf
 *     → khởi tạo lock, waitqueue, workqueue
 *     → rtl8188_chrdev_init()   — tạo /dev/rtl8188
 *     → rtl8188_proc_init()     — tạo /proc/rtl8188/
 *     → rtl8188_usb_init()      — đăng ký USB notifier
 *     → rtl8188_netdev_init()   — đăng ký netdev notifier
 *     → quét USB và netdev đang có sẵn
 */

#include "rtl8188_mon.h"
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/kmod.h>
#include <linux/fs.h>

/* ================================================================
 * Biến toàn cục
 * ================================================================ */

/*
 * g_mon — Instance duy nhất của module state.
 * Được cấp phát khi module load, giải phóng khi unload.
 * Tất cả file con truy cập thông qua extern khai báo trong header.
 */
struct rtl8188_mon *g_mon;

/*
 * helper_envp — Môi trường truyền vào call_usermodehelper().
 *
 * Cần thiết để các lệnh như iw, wpa_supplicant, nmcli hoạt động
 * đúng khi được gọi từ kernel context:
 *   - PATH: tìm được các binary hệ thống
 *   - DBUS_SYSTEM_BUS_ADDRESS: nmcli kết nối được D-Bus system bus
 *   - XDG_RUNTIME_DIR: một số daemon cần biến này
 */
char *helper_envp[] = {
	"HOME=/",
	"TERM=linux",
	"PATH=/sbin:/bin:/usr/sbin:/usr/bin",
	"DBUS_SYSTEM_BUS_ADDRESS=unix:path=/run/dbus/system_bus_socket",
	"XDG_RUNTIME_DIR=/run",
	NULL
};

/* ================================================================
 * Hàm tiện ích dùng chung
 * ================================================================ */

/**
 * run_cmd() - Chạy lệnh shell qua /bin/sh -c từ kernel context
 *
 * Dùng UMH_WAIT_PROC để block đến khi lệnh kết thúc.
 * SELinux phải ở Permissive mode (setenforce 0).
 */
int run_cmd(const char *cmd)
{
	char *argv[] = { "/bin/sh", "-c", (char *)cmd, NULL };

	return call_usermodehelper(argv[0], argv, helper_envp, UMH_WAIT_PROC);
}

/**
 * read_tmpfile() - Đọc file tạm từ filesystem vào buffer kernel
 *
 * Dùng kernel_read() thay vì filp_read() để tương thích với
 * các phiên bản kernel mới. Luôn null-terminate kết quả.
 */
int read_tmpfile(const char *path, char *buf, int size)
{
	struct file *f;
	loff_t pos = 0;
	ssize_t n;

	f = filp_open(path, O_RDONLY, 0);
	if (IS_ERR(f))
		return 0;

	n = kernel_read(f, buf, size - 1, &pos);
	filp_close(f, NULL);

	if (n < 0)
		n = 0;
	buf[n] = '\0';
	return (int)n;
}

/**
 * sanitize() - Làm sạch chuỗi để tránh shell command injection
 *
 * Thay thế các ký tự đặc biệt của shell bằng '_'.
 * Gọi trước khi nhúng SSID/password vào lệnh shell trong connect_work_fn().
 */
void sanitize(char *s, int len)
{
	int i;

	for (i = 0; i < len && s[i]; i++) {
		char c = s[i];

		if (c == '\'' || c == '"' || c == '`' || c == '$' ||
		    c == ';'  || c == '|' || c == '&' || c == '\\' ||
		    c == '('  || c == ')' || c == '<' || c == '>')
			s[i] = '_';
	}
}

/**
 * set_resp() - Ghi phản hồi text vào resp_buf và thức dậy read()
 *
 * Hàm thread-safe: tự giữ cmd_lock.
 * Gọi wake_up_interruptible() sau khi ghi xong để unblock các
 * tiến trình đang wait_event_interruptible_timeout() trong read().
 */
void set_resp(struct rtl8188_mon *mon, const char *text)
{
	mutex_lock(&mon->cmd_lock);
	mon->resp_len = min_t(int, (int)strlen(text), RESP_BUF_SIZE - 1);
	memcpy(mon->resp_buf, text, mon->resp_len);
	mon->resp_buf[mon->resp_len] = '\0';
	mon->resp_ready = true;
	mutex_unlock(&mon->cmd_lock);
	wake_up_interruptible(&mon->resp_wq);
}

/* ================================================================
 * Module init / exit
 * ================================================================ */

/**
 * rtl8188_mon_init() - Hàm khởi tạo khi insmod
 *
 * Thứ tự khởi tạo quan trọng:
 *  1. Cấp phát bộ nhớ cho g_mon và resp_buf
 *  2. Khởi tạo các cơ chế đồng bộ (mutex, spinlock, waitqueue)
 *  3. Khởi tạo workqueue và các work item
 *  4. Tạo char device /dev/rtl8188
 *  5. Tạo /proc/rtl8188/ entries
 *  6. Đăng ký USB notifier (phát hiện device)
 *  7. Đăng ký netdev notifier (phát hiện interface)
 *  8. Quét các device/interface đang có sẵn trước khi module load
 */
static int __init rtl8188_mon_init(void)
{
	int ret;

	/* Cấp phát cấu trúc trạng thái chính, zero-initialized */
	g_mon = kzalloc(sizeof(*g_mon), GFP_KERNEL);
	if (!g_mon)
		return -ENOMEM;

	/* Cấp phát buffer lệnh/phản hồi riêng để tránh chiếm stack */
	g_mon->resp_buf = kzalloc(RESP_BUF_SIZE, GFP_KERNEL);
	if (!g_mon->resp_buf) {
		ret = -ENOMEM;
		goto err_free_mon;
	}

	/* Khởi tạo các cơ chế đồng bộ */
	mutex_init(&g_mon->cmd_lock);
	spin_lock_init(&g_mon->ring_lock);
	init_waitqueue_head(&g_mon->resp_wq);

	/* Liên kết các work item với hàm xử lý tương ứng */
	INIT_WORK(&g_mon->scan_work,       scan_work_fn);
	INIT_WORK(&g_mon->connect_work,    connect_work_fn);
	INIT_WORK(&g_mon->disconnect_work, disconnect_work_fn);

	/* Ghi nhớ thời điểm load để tính module uptime */
	g_mon->load_jiffies = jiffies;

	/*
	 * Tạo single-thread workqueue để đảm bảo các lệnh async
	 * (scan, connect, disconnect) chạy tuần tự, không xung đột.
	 */
	g_mon->wq = create_singlethread_workqueue("rtl8188_mon");
	if (!g_mon->wq) {
		ret = -ENOMEM;
		goto err_free_buf;
	}

	/* Tạo character device /dev/rtl8188 */
	ret = rtl8188_chrdev_init();
	if (ret)
		goto err_destroy_wq;

	/* Tạo /proc/rtl8188/ và các entry con */
	ret = rtl8188_proc_init();
	if (ret)
		goto err_chrdev;

	/* Đăng ký USB notifier — phát hiện plug/unplug RTL8188ETV */
	rtl8188_usb_init();

	/* Đăng ký netdev notifier — theo dõi interface wlan lên/xuống */
	rtl8188_netdev_init();

	/*
	 * Quét các device đã có sẵn trước khi module load.
	 * Trường hợp: người dùng cắm USB trước rồi mới insmod.
	 */
	if (g_mon->dev_present)
		scan_existing_netdev();

	pr_info("[rtl8188_mon] Module loaded. "
		"Device: /dev/%s  Proc: /proc/%s/\n",
		DEVICE_NAME, DEVICE_NAME);

	if (g_mon->dev_present)
		pr_info("[rtl8188_mon] RTL8188ETV đã được tìm thấy, interface: %s\n",
			g_mon->ifname[0] ? g_mon->ifname : "(đang chờ)");

	return 0;

	/* Cleanup theo thứ tự ngược nếu có lỗi */
err_chrdev:
	rtl8188_chrdev_exit();
err_destroy_wq:
	destroy_workqueue(g_mon->wq);
err_free_buf:
	kfree(g_mon->resp_buf);
err_free_mon:
	kfree(g_mon);
	g_mon = NULL;
	return ret;
}

/**
 * rtl8188_mon_exit() - Hàm dọn dẹp khi rmmod
 *
 * Thứ tự dọn dẹp ngược với init:
 *  1. Dừng packet monitoring nếu đang chạy
 *  2. Hủy các work đang pending và destroy workqueue
 *  3. Hủy đăng ký netdev và USB notifier
 *  4. Xóa /proc/rtl8188/ entries
 *  5. Xóa /dev/rtl8188
 *  6. Giải phóng USB device reference
 *  7. Dọn file tạm và giải phóng bộ nhớ
 */
static void __exit rtl8188_mon_exit(void)
{
	if (!g_mon)
		return;

	/* Dừng packet monitoring trước tiên để không còn callback */
	if (g_mon->pkt_registered)
		dev_remove_pack(&g_mon->ptype);

	/*
	 * Hủy các work đang trong queue và chờ work đang chạy kết thúc.
	 * cancel_work_sync() đảm bảo an toàn khi destroy workqueue.
	 */
	cancel_work_sync(&g_mon->scan_work);
	cancel_work_sync(&g_mon->connect_work);
	cancel_work_sync(&g_mon->disconnect_work);
	destroy_workqueue(g_mon->wq);

	/* Hủy đăng ký các notifier */
	rtl8188_netdev_exit();
	rtl8188_usb_exit();

	/* Xóa /proc/rtl8188/ */
	rtl8188_proc_exit();

	/* Xóa /dev/rtl8188 */
	rtl8188_chrdev_exit();

	/* Giải phóng reference tới USB device */
	if (g_mon->udev)
		usb_put_dev(g_mon->udev);

	/* Xóa các file tạm (fire-and-forget, không cần chờ) */
	run_cmd("rm -f " SCAN_FILE " " STATUS_FILE " " CONNECT_FILE
		" /tmp/.rtl8188_wpa.conf 2>/dev/null");

	kfree(g_mon->resp_buf);
	kfree(g_mon);
	g_mon = NULL;

	pr_info("[rtl8188_mon] Module unloaded\n");
}

module_init(rtl8188_mon_init);
module_exit(rtl8188_mon_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Student Project");
MODULE_DESCRIPTION("RTL8188ETV WiFi Companion Monitor Driver");
MODULE_VERSION("1.0");