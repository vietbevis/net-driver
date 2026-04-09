// SPDX-License-Identifier: GPL-2.0
/*
 * RTL8188ETV WiFi Companion Monitor — Character Device /dev/rtl8188
 *
 * Cung cấp giao diện giao tiếp giữa userspace và kernel module qua
 * character device /dev/rtl8188.
 *
 * Giao thức:
 *   write() — Userspace gửi lệnh text (vd: "scan", "monitor on")
 *   read()  — Userspace đọc kết quả; block tối đa 60s nếu chưa có sẵn
 *
 * Các lệnh sync (trả kết quả ngay trong write()):
 *   info, status, stats, capture, monitor on/off, filter *
 *
 * Các lệnh async (trả kết quả sau khi workqueue hoàn tất):
 *   scan, connect, disconnect
 *
 * File position (f_pos) được reset về 0 sau mỗi lệnh write() để
 * read() lần sau luôn đọc từ đầu buffer.
 */

#include "rtl8188_mon.h"
#include <linux/uaccess.h>
#include <linux/slab.h>

/* ================================================================
 * File operations
 * ================================================================ */

/**
 * rtl8188_dev_open() - Callback khi userspace mở /dev/rtl8188
 *
 * Không cần khởi tạo gì thêm — state nằm trong g_mon toàn cục.
 */
static int rtl8188_dev_open(struct inode *inode, struct file *f)
{
	return 0;
}

/**
 * rtl8188_dev_release() - Callback khi userspace đóng file descriptor
 */
static int rtl8188_dev_release(struct inode *inode, struct file *f)
{
	return 0;
}

/**
 * rtl8188_dev_read() - Đọc kết quả lệnh từ resp_buf về userspace
 * @f:   File descriptor
 * @buf: Buffer userspace để ghi vào
 * @len: Số byte userspace muốn đọc
 * @off: File position (offset trong resp_buf)
 *
 * Nếu resp_ready chưa true:
 *   - O_NONBLOCK: trả -EAGAIN ngay
 *   - Blocking: đợi tối đa 60 giây qua wait_event_interruptible_timeout()
 *
 * Hỗ trợ partial read: read() nhiều lần sẽ tiếp tục từ *off.
 * Khi *off >= resp_len: trả 0 (EOF).
 */
static ssize_t rtl8188_dev_read(struct file *f, char __user *buf,
				size_t len, loff_t *off)
{
	struct rtl8188_mon *mon = g_mon;
	int avail, ret;

	if (!mon)
		return -ENODEV;

	/* Nếu chưa có kết quả, chờ hoặc trả về ngay */
	if (!mon->resp_ready) {
		if (f->f_flags & O_NONBLOCK)
			return -EAGAIN;

		ret = wait_event_interruptible_timeout(
			mon->resp_wq,
			mon->resp_ready,
			msecs_to_jiffies(60000));  /* timeout 60 giây */

		if (ret == 0)
			return -ETIMEDOUT;
		if (ret < 0)
			return ret;  /* bị signal interrupt */
	}

	mutex_lock(&mon->cmd_lock);

	avail = mon->resp_len - (int)*off;
	if (avail <= 0) {
		/* EOF — đã đọc hết hoặc buffer trống */
		mutex_unlock(&mon->cmd_lock);
		return 0;
	}

	/* Giới hạn số byte trả về theo len và avail */
	ret = min_t(int, (int)len, avail);

	if (copy_to_user(buf, mon->resp_buf + *off, ret)) {
		mutex_unlock(&mon->cmd_lock);
		return -EFAULT;
	}

	*off += ret;
	mutex_unlock(&mon->cmd_lock);
	return ret;
}

/**
 * rtl8188_dev_write() - Nhận lệnh từ userspace và dispatch
 * @f:   File descriptor
 * @buf: Buffer chứa lệnh từ userspace
 * @len: Độ dài lệnh
 * @off: File position (bị reset về 0 sau mỗi lệnh)
 *
 * Xóa resp_ready trước khi xử lý để read() phải chờ kết quả mới.
 * Trim newline/carriage return ở cuối lệnh trước khi so sánh.
 */
static ssize_t rtl8188_dev_write(struct file *f, const char __user *buf,
				 size_t len, loff_t *off)
{
	struct rtl8188_mon *mon = g_mon;
	char cmd[MAX_CMD_LEN];
	int n;

	if (!mon)
		return -ENODEV;

	/* Sao chép lệnh từ userspace, giới hạn MAX_CMD_LEN-1 byte */
	n = min_t(int, (int)len, MAX_CMD_LEN - 1);
	if (copy_from_user(cmd, buf, n))
		return -EFAULT;
	cmd[n] = '\0';

	/* Xóa newline/carriage return ở cuối */
	while (n > 0 && (cmd[n - 1] == '\n' || cmd[n - 1] == '\r'))
		cmd[--n] = '\0';

	/*
	 * Đặt resp_ready = false để buộc read() phải chờ kết quả mới.
	 * Quan trọng: phải làm trước khi dispatch lệnh.
	 */
	mutex_lock(&mon->cmd_lock);
	mon->resp_ready = false;
	mon->resp_len   = 0;
	mutex_unlock(&mon->cmd_lock);

	/* ---- Dispatch lệnh ---- */

	if (strcmp(cmd, "scan") == 0) {
		/* Async: kết quả sẵn sàng sau khi iw scan hoàn tất */
		queue_work(mon->wq, &mon->scan_work);

	} else if (strncmp(cmd, "connect ", 8) == 0) {
		/* Async: parse SSID [password] rồi queue connect_work */
		char *args = cmd + 8;
		char *sp   = strchr(args, ' ');

		if (sp) {
			*sp = '\0';
			strscpy(mon->cmd_ssid, args, sizeof(mon->cmd_ssid));
			strscpy(mon->cmd_pass, sp + 1, sizeof(mon->cmd_pass));
		} else {
			strscpy(mon->cmd_ssid, args, sizeof(mon->cmd_ssid));
			mon->cmd_pass[0] = '\0';
		}
		/* Làm sạch SSID và password để tránh shell injection */
		sanitize(mon->cmd_ssid, sizeof(mon->cmd_ssid));
		sanitize(mon->cmd_pass, sizeof(mon->cmd_pass));
		queue_work(mon->wq, &mon->connect_work);

	} else if (strcmp(cmd, "disconnect") == 0) {
		/* Async: dừng wpa_supplicant và iw disconnect */
		queue_work(mon->wq, &mon->disconnect_work);

	} else if (strcmp(cmd, "monitor on") == 0) {
		/* Sync: đăng ký packet handler nếu chưa có */
		if (!mon->pkt_registered && mon->ndev) {
			mon->ptype.type = cpu_to_be16(ETH_P_ALL);
			mon->ptype.func = rtl8188_pkt_recv;
			mon->ptype.dev  = mon->ndev;
			dev_add_pack(&mon->ptype);
			mon->pkt_registered = true;
		}
		mon->capture_on = true;
		set_resp(mon, "Packet monitoring: ON\n");

	} else if (strcmp(cmd, "monitor off") == 0) {
		/* Sync: hủy đăng ký packet handler */
		mon->capture_on = false;
		if (mon->pkt_registered) {
			dev_remove_pack(&mon->ptype);
			mon->pkt_registered = false;
		}
		set_resp(mon, "Packet monitoring: OFF\n");

	} else if (strcmp(cmd, "status") == 0) {
		generate_status(mon);

	} else if (strcmp(cmd, "info") == 0) {
		generate_info(mon);

	} else if (strcmp(cmd, "stats") == 0) {
		generate_stats(mon);

	} else if (strcmp(cmd, "capture") == 0) {
		generate_capture(mon);

	} else if (strcmp(cmd, "filter chat") == 0) {
		/* Sync: lọc chỉ port 9090 (CryptoChat) */
		mon->filter_port = htons(CHAT_PORT);
		atomic_set(&mon->filter_cnt, 0);
		set_resp(mon, "Filter: chỉ port 9090 (CryptoChat)\n");

	} else if (strncmp(cmd, "filter ", 7) == 0) {
		/* Sync: lọc theo port number hoặc xóa filter (port 0) */
		unsigned int port;

		if (kstrtouint(cmd + 7, 10, &port) == 0 && port <= 65535) {
			mon->filter_port = htons(port);
			atomic_set(&mon->filter_cnt, 0);
			if (port == 0) {
				set_resp(mon, "Filter: đã tắt (bắt tất cả)\n");
			} else {
				char tmp[80];
				snprintf(tmp, sizeof(tmp),
					 "Filter: chỉ port %u\n", port);
				set_resp(mon, tmp);
			}
		} else {
			set_resp(mon, "Cú pháp: filter <port> hoặc filter chat\n");
		}

	} else if (strcmp(cmd, "filter") == 0) {
		/* Sync: hiển thị filter đang active */
		char tmp[80];

		if (mon->filter_port)
			snprintf(tmp, sizeof(tmp),
				 "Filter hiện tại: port %d\n",
				 ntohs(mon->filter_port));
		else
			snprintf(tmp, sizeof(tmp),
				 "Không có filter (đang bắt tất cả)\n");
		set_resp(mon, tmp);

	} else {
		/* Lệnh không nhận dạng được: trả về help text */
		set_resp(mon,
			 "============================================\n"
			 "        RTL8188 Monitor - Commands\n"
			 "============================================\n\n"
			 "  scan              Quét WiFi\n"
			 "  connect SSID [PW] Kết nối mạng\n"
			 "  disconnect        Ngắt kết nối\n"
			 "  status            Trạng thái kết nối\n"
			 "  info              Thông tin thiết bị\n"
			 "  stats             Thống kê gói tin\n"
			 "  monitor on|off    Bật/tắt packet monitoring\n"
			 "  capture           Xem gói tin đã bắt\n"
			 "  filter <port>     Lọc theo port\n"
			 "  filter chat       Lọc CryptoChat (9090)\n"
			 "  filter 0          Xóa filter\n");
	}

	/*
	 * Reset file position về 0 để lần read() tiếp theo
	 * luôn đọc từ đầu resp_buf, kể cả khi userspace không close/reopen.
	 */
	*off = 0;
	return len;
}

/* ================================================================
 * File operations table
 * ================================================================ */

static const struct file_operations rtl8188_fops = {
	.owner   = THIS_MODULE,
	.open    = rtl8188_dev_open,
	.release = rtl8188_dev_release,
	.read    = rtl8188_dev_read,
	.write   = rtl8188_dev_write,
};

/* ================================================================
 * Init / exit
 * ================================================================ */

/**
 * rtl8188_chrdev_init() - Tạo character device /dev/rtl8188
 *
 * Thứ tự:
 *   1. alloc_chrdev_region()  — cấp major/minor number động
 *   2. cdev_init() + cdev_add() — đăng ký file_operations
 *   3. class_create()         — tạo device class trong sysfs
 *   4. device_create()        — tạo node /dev/rtl8188 qua udev
 *
 * Trả về: 0 thành công, errno âm nếu lỗi.
 */
int rtl8188_chrdev_init(void)
{
	int ret;

	ret = alloc_chrdev_region(&g_mon->devno, 0, 1, DEVICE_NAME);
	if (ret) {
		pr_err("[rtl8188_mon] Không cấp được chrdev region: %d\n", ret);
		return ret;
	}

	cdev_init(&g_mon->cdev, &rtl8188_fops);
	g_mon->cdev.owner = THIS_MODULE;

	ret = cdev_add(&g_mon->cdev, g_mon->devno, 1);
	if (ret) {
		pr_err("[rtl8188_mon] cdev_add thất bại: %d\n", ret);
		goto err_region;
	}

	g_mon->cls = class_create(DEVICE_NAME);
	if (IS_ERR(g_mon->cls)) {
		ret = PTR_ERR(g_mon->cls);
		pr_err("[rtl8188_mon] class_create thất bại: %d\n", ret);
		goto err_cdev;
	}

	g_mon->chrdev = device_create(g_mon->cls, NULL, g_mon->devno,
				      NULL, DEVICE_NAME);
	if (IS_ERR(g_mon->chrdev)) {
		ret = PTR_ERR(g_mon->chrdev);
		pr_err("[rtl8188_mon] device_create thất bại: %d\n", ret);
		goto err_class;
	}

	return 0;

err_class:
	class_destroy(g_mon->cls);
err_cdev:
	cdev_del(&g_mon->cdev);
err_region:
	unregister_chrdev_region(g_mon->devno, 1);
	return ret;
}

/**
 * rtl8188_chrdev_exit() - Xóa character device /dev/rtl8188
 *
 * Thứ tự ngược với init để tránh race condition.
 */
void rtl8188_chrdev_exit(void)
{
	device_destroy(g_mon->cls, g_mon->devno);
	class_destroy(g_mon->cls);
	cdev_del(&g_mon->cdev);
	unregister_chrdev_region(g_mon->devno, 1);
}