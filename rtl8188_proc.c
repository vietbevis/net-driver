// SPDX-License-Identifier: GPL-2.0
/*
 * RTL8188ETV WiFi Companion Monitor — Proc Filesystem
 *
 * Tạo /proc/rtl8188/ với 4 entry chỉ đọc (0444):
 *
 *   /proc/rtl8188/device  — Thông tin USB device (vendor, product, MAC)
 *   /proc/rtl8188/stats   — Thống kê gói tin real-time
 *   /proc/rtl8188/status  — Trạng thái kết nối WiFi (iw link)
 *   /proc/rtl8188/scan    — Kết quả quét WiFi lần cuối
 *
 * Dùng proc_create_single() + seq_file interface (kernel 5.6+).
 * Không cần file_operations riêng — seq_file tự xử lý read/seek.
 */

#include "rtl8188_mon.h"
#include <linux/seq_file.h>
#include <linux/proc_fs.h>
#include <linux/slab.h>

/**
 * proc_device_show() - Hiển thị thông tin USB device tại /proc/rtl8188/device
 * @m: seq_file để ghi output
 * @v: Không dùng (always NULL cho single entry)
 */
static int proc_device_show(struct seq_file *m, void *v)
{
	if (!g_mon)
		return 0;

	seq_puts(m, "=== RTL8188ETV USB WiFi Device ===\n\n");

	if (g_mon->dev_present && g_mon->udev) {
		struct usb_device *u = g_mon->udev;

		seq_printf(m, "Vendor:     0x%04x (%s)\n",
			   le16_to_cpu(u->descriptor.idVendor),
			   u->manufacturer ? u->manufacturer : "Realtek");
		seq_printf(m, "Product:    0x%04x (%s)\n",
			   le16_to_cpu(u->descriptor.idProduct),
			   u->product ? u->product : "RTL8188ETV");
		seq_printf(m, "USB Speed:  %s\n",
			   usb_speed_string(u->speed));
		seq_printf(m, "Interface:  %s\n",
			   g_mon->ifname[0] ? g_mon->ifname : "(none)");
		if (g_mon->ndev)
			seq_printf(m, "MAC:        %pM\n",
				   g_mon->ndev->dev_addr);
	} else {
		seq_puts(m, "Status: Không tìm thấy thiết bị\n");
	}

	seq_printf(m, "Uptime:     %lu giây\n",
		   (jiffies - g_mon->load_jiffies) / HZ);
	return 0;
}

/**
 * proc_stats_show() - Hiển thị thống kê gói tin tại /proc/rtl8188/stats
 * @m: seq_file để ghi output
 * @v: Không dùng
 *
 * Đọc trực tiếp từ atomic counter — không cần lock.
 * Cũng lấy thống kê từ driver (rtl8xxxu) qua dev_get_stats().
 */
static int proc_stats_show(struct seq_file *m, void *v)
{
	struct rtnl_link_stats64 ns;

	if (!g_mon)
		return 0;

	seq_puts(m, "=== Packet Statistics ===\n\n");
	seq_printf(m, "Monitor: %s  |  Capture: %s\n\n",
		   g_mon->pkt_registered ? "ON" : "OFF",
		   g_mon->capture_on     ? "ON" : "OFF");

	if (g_mon->pkt_registered) {
		seq_printf(m,
			   "Monitored RX: %lld pkts, %lld bytes\n"
			   "  ARP=%d  IPv4=%d  IPv6=%d  Other=%d\n"
			   "  TCP=%d  UDP=%d  ICMP=%d  Chat(9090)=%d\n\n",
			   atomic64_read(&g_mon->rx_pkts),
			   atomic64_read(&g_mon->rx_bytes),
			   atomic_read(&g_mon->arp_cnt),
			   atomic_read(&g_mon->ip_cnt),
			   atomic_read(&g_mon->ipv6_cnt),
			   atomic_read(&g_mon->other_cnt),
			   atomic_read(&g_mon->tcp_cnt),
			   atomic_read(&g_mon->udp_cnt),
			   atomic_read(&g_mon->icmp_cnt),
			   atomic_read(&g_mon->chat_cnt));
	}

	if (g_mon->ndev) {
		dev_get_stats(g_mon->ndev, &ns);
		seq_printf(m,
			   "Driver TX: %llu pkts, %llu bytes\n"
			   "Driver RX: %llu pkts, %llu bytes\n",
			   ns.tx_packets, ns.tx_bytes,
			   ns.rx_packets, ns.rx_bytes);
	}

	seq_printf(m, "\nUptime: %lu giây\n",
		   (jiffies - g_mon->load_jiffies) / HZ);
	return 0;
}

/**
 * proc_status_show() - Hiển thị trạng thái kết nối tại /proc/rtl8188/status
 * @m: seq_file để ghi output
 * @v: Không dùng
 *
 * Gọi `iw dev <iface> link` qua shell, đọc kết quả từ STATUS_FILE.
 */
static int proc_status_show(struct seq_file *m, void *v)
{
	char cmd[256];
	char *buf;
	int n;

	if (!g_mon || !g_mon->ifname[0]) {
		seq_puts(m, "Không có interface\n");
		return 0;
	}

	buf = kmalloc(4096, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	snprintf(cmd, sizeof(cmd),
		 "/usr/sbin/iw dev %s link > " STATUS_FILE " 2>&1",
		 g_mon->ifname);
	run_cmd(cmd);

	n = read_tmpfile(STATUS_FILE, buf, 4096);
	if (n > 0)
		seq_printf(m, "%s", buf);
	else
		seq_puts(m, "Không có kết nối\n");

	kfree(buf);
	return 0;
}

/**
 * proc_scan_show() - Hiển thị kết quả scan cuối tại /proc/rtl8188/scan
 * @m: seq_file để ghi output
 * @v: Không dùng
 *
 * In nội dung resp_buf hiện tại (nếu là kết quả scan).
 * Không thực hiện scan mới — chỉ đọc cache.
 */
static int proc_scan_show(struct seq_file *m, void *v)
{
	if (!g_mon)
		return 0;

	mutex_lock(&g_mon->cmd_lock);
	if (g_mon->resp_len > 0)
		seq_printf(m, "%s", g_mon->resp_buf);
	else
		seq_puts(m, "Chưa có kết quả scan. Chạy: echo scan > /dev/rtl8188\n");
	mutex_unlock(&g_mon->cmd_lock);
	return 0;
}

/* ================================================================
 * Init / exit
 * ================================================================ */

/**
 * rtl8188_proc_init() - Tạo thư mục /proc/rtl8188/ và các entry con
 *
 * Dùng proc_mkdir() để tạo thư mục, proc_create_single() cho từng entry.
 * Nếu proc_mkdir() thất bại (rất hiếm), trả lỗi.
 *
 * Trả về: 0 thành công, -ENOMEM nếu không tạo được thư mục.
 */
int rtl8188_proc_init(void)
{
	g_mon->proc_dir = proc_mkdir(DEVICE_NAME, NULL);
	if (!g_mon->proc_dir) {
		pr_err("[rtl8188_mon] Không tạo được /proc/%s/\n", DEVICE_NAME);
		return -ENOMEM;
	}

	proc_create_single("device", 0444, g_mon->proc_dir, proc_device_show);
	proc_create_single("stats",  0444, g_mon->proc_dir, proc_stats_show);
	proc_create_single("status", 0444, g_mon->proc_dir, proc_status_show);
	proc_create_single("scan",   0444, g_mon->proc_dir, proc_scan_show);

	return 0;
}

/**
 * rtl8188_proc_exit() - Xóa các proc entry và thư mục /proc/rtl8188/
 *
 * Phải xóa các entry con trước khi xóa thư mục cha.
 */
void rtl8188_proc_exit(void)
{
	if (!g_mon->proc_dir)
		return;

	remove_proc_entry("device", g_mon->proc_dir);
	remove_proc_entry("stats",  g_mon->proc_dir);
	remove_proc_entry("status", g_mon->proc_dir);
	remove_proc_entry("scan",   g_mon->proc_dir);
	remove_proc_entry(DEVICE_NAME, NULL);
}