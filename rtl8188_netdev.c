// SPDX-License-Identifier: GPL-2.0
/*
 * RTL8188ETV WiFi Companion Monitor — Network Interface Tracking
 *
 * Theo dõi trạng thái của wireless interface (wlan*) do rtl8xxxu tạo ra.
 * Dùng register_netdevice_notifier() để nhận sự kiện REGISTER, UP, DOWN,
 * UNREGISTER, CHANGENAME từ Linux networking stack.
 *
 * Quan trọng: is_our_iface() xác định interface thuộc về RTL8188ETV
 * bằng cách leo cây device từ net_device lên đến usb_device đang lưu.
 */

#include "rtl8188_mon.h"
#include <linux/rtnetlink.h>

/**
 * is_our_iface() - Kiểm tra net_device có thuộc về RTL8188ETV không
 * @ndev: Network device cần kiểm tra
 *
 * Leo cây device hierarchy từ ndev->dev.parent trở lên.
 * Nếu tìm thấy &g_mon->udev->dev trong cây, interface này
 * được tạo bởi driver (rtl8xxxu) quản lý RTL8188ETV của ta.
 *
 * Trả về: true nếu đây là interface của ta, false nếu không.
 */
static bool is_our_iface(struct net_device *ndev)
{
	struct device *d;

	/* Nếu chưa có USB device, không thể xác định */
	if (!g_mon || !g_mon->udev)
		return false;

	for (d = ndev->dev.parent; d; d = d->parent) {
		if (d == &g_mon->udev->dev)
			return true;
	}
	return false;
}

/**
 * rtl8188_net_notify() - Callback nhận sự kiện thay đổi network interface
 * @nb:    Notifier block (embedded trong g_mon->net_nb)
 * @event: Loại sự kiện (NETDEV_REGISTER, NETDEV_UP, ...)
 * @ptr:   netdev_notifier_info chứa con trỏ tới net_device
 *
 * Chỉ xử lý các interface được xác định là thuộc về RTL8188ETV.
 */
static int rtl8188_net_notify(struct notifier_block *nb,
			      unsigned long event, void *ptr)
{
	struct net_device *dev = netdev_notifier_info_to_dev(ptr);

	if (!g_mon || !is_our_iface(dev))
		return NOTIFY_DONE;

	switch (event) {
	case NETDEV_REGISTER:
		/*
		 * Interface vừa được tạo bởi rtl8xxxu.
		 * Lưu lại để có thể truyền tên interface vào lệnh iw.
		 */
		g_mon->ndev = dev;
		strscpy(g_mon->ifname, dev->name, IFNAMSIZ);
		pr_info("[rtl8188_mon] Interface %s đã được đăng ký\n",
			dev->name);
		break;

	case NETDEV_UP:
		/* Interface đã được bật (ip link set up) */
		g_mon->ndev = dev;
		strscpy(g_mon->ifname, dev->name, IFNAMSIZ);
		pr_info("[rtl8188_mon] Interface %s đang UP\n", dev->name);
		break;

	case NETDEV_DOWN:
		pr_info("[rtl8188_mon] Interface %s đang DOWN\n", dev->name);
		break;

	case NETDEV_UNREGISTER:
		/*
		 * Interface sắp bị xóa (thường do USB bị rút).
		 * Dừng packet monitoring và xóa con trỏ để tránh use-after-free.
		 */
		if (g_mon->pkt_registered) {
			dev_remove_pack(&g_mon->ptype);
			g_mon->pkt_registered = false;
		}
		g_mon->ndev      = NULL;
		g_mon->ifname[0] = '\0';
		pr_info("[rtl8188_mon] Interface đã bị hủy đăng ký\n");
		break;

	case NETDEV_CHANGENAME:
		/* Interface được đổi tên (vd: wlan0 → wlan1) */
		strscpy(g_mon->ifname, dev->name, IFNAMSIZ);
		pr_info("[rtl8188_mon] Interface được đổi tên thành %s\n",
			dev->name);
		break;
	}

	return NOTIFY_OK;
}

/**
 * scan_existing_netdev() - Quét để tìm interface wlan đang có sẵn
 *
 * Gọi khi:
 *   (1) Module load và RTL8188ETV đã được cắm trước đó
 *   (2) USB_DEVICE_ADD notification nhận được
 *
 * Dùng rtnl_lock() để an toàn khi duyệt danh sách net_device.
 * Chỉ lưu interface đầu tiên tìm thấy (thường chỉ có một).
 */
void scan_existing_netdev(void)
{
	struct net_device *dev;

	rtnl_lock();
	for_each_netdev(&init_net, dev) {
		if (is_our_iface(dev)) {
			g_mon->ndev = dev;
			strscpy(g_mon->ifname, dev->name, IFNAMSIZ);
			pr_info("[rtl8188_mon] Tìm thấy interface có sẵn: %s\n",
				dev->name);
			break;
		}
	}
	rtnl_unlock();
}

/**
 * rtl8188_netdev_init() - Đăng ký netdev notifier
 *
 * Gọi trong rtl8188_mon_init() sau khi USB notifier đã được đăng ký.
 */
void rtl8188_netdev_init(void)
{
	g_mon->net_nb.notifier_call = rtl8188_net_notify;
	register_netdevice_notifier(&g_mon->net_nb);
}

/**
 * rtl8188_netdev_exit() - Hủy đăng ký netdev notifier
 *
 * Gọi trong rtl8188_mon_exit() trước khi dọn dẹp char device và proc.
 */
void rtl8188_netdev_exit(void)
{
	unregister_netdevice_notifier(&g_mon->net_nb);
}