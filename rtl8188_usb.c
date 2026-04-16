// SPDX-License-Identifier: GPL-2.0
/*
 * RTL8188ETV WiFi Companion Monitor — USB Device Detection
 *
 * Module này KHÔNG dùng usb_register() để tránh xung đột với driver
 * hệ thống rtl8xxxu đang claim thiết bị. Thay vào đó, chỉ dùng
 * usb_register_notify() để quan sát thụ động sự kiện plug/unplug.
 *
 * Khi phát hiện thiết bị RTL8188ETV (0bda:0179):
 *   - Lưu con trỏ USB device (tăng reference count qua usb_get_dev)
 *   - Gọi scan_existing_netdev() để tìm interface wlan tương ứng
 *
 * Khi thiết bị bị rút:
 *   - Dừng packet monitoring nếu đang chạy
 *   - Giải phóng USB device reference
 */

#include "rtl8188_mon.h"
#include <linux/usb.h>

/**
 * find_our_usb() - Callback cho usb_for_each_dev() khi module load
 * @udev: USB device đang được kiểm tra trong vòng lặp
 * @data: Không dùng (NULL)
 *
 * Hàm này được gọi cho từng USB device hiện có trên bus.
 * Nếu tìm thấy RTL8188ETV, lưu reference và dừng vòng lặp (return 1).
 *
 * Trả về: 1 nếu tìm thấy thiết bị cần (dừng vòng lặp), 0 nếu không.
 */
static int find_our_usb(struct usb_device *udev, void *data)
{
	if (le16_to_cpu(udev->descriptor.idVendor)  != TARGET_VENDOR ||
	    le16_to_cpu(udev->descriptor.idProduct) != TARGET_PRODUCT)
		return 0;

	/* Tăng reference count để device không bị giải phóng khi ta đang dùng */
	g_mon->udev = usb_get_dev(udev);
	g_mon->dev_present = true;
	return 1; /* Dừng vòng lặp */
}

/**
 * rtl8188_usb_notify() - Callback nhận sự kiện USB plug/unplug
 * @nb:     Notifier block (embedded trong g_mon->usb_nb)
 * @action: USB_DEVICE_ADD hoặc USB_DEVICE_REMOVE
 * @data:   Con trỏ tới struct usb_device
 *
 * Hàm được gọi trong notifier chain khi có USB device được thêm/xóa.
 * Chỉ xử lý device có VID=0x0bda, PID=0x0179.
 */
static int rtl8188_usb_notify(struct notifier_block *nb,
			      unsigned long action, void *data)
{
	struct usb_device *udev = data;

	/* Bỏ qua các device không phải RTL8188ETV */
	if (le16_to_cpu(udev->descriptor.idVendor)  != TARGET_VENDOR ||
	    le16_to_cpu(udev->descriptor.idProduct)  != TARGET_PRODUCT)
		return NOTIFY_DONE;

	switch (action) {
	case USB_DEVICE_ADD:
		/* Chỉ xử lý nếu chưa có device (tránh duplicate) */
		if (!g_mon->dev_present) {
			g_mon->udev = usb_get_dev(udev);
			g_mon->dev_present = true;
			pr_info("[rtl8188_mon] RTL8188ETV đã được cắm vào\n");
			/* Tìm interface wlan vừa được tạo bởi rtl8xxxu */
			scan_existing_netdev();
		}
		break;

	case USB_DEVICE_REMOVE:
		if (g_mon->udev != udev)
			break;

		/* Dừng packet monitoring trước khi mất interface */
		if (g_mon->pkt_registered) {
			dev_remove_pack(&g_mon->ptype);
			g_mon->pkt_registered = false;
		}

		/* Xóa thông tin interface đã không còn hợp lệ */
		g_mon->ndev      = NULL;
		g_mon->ifname[0] = '\0';

		/* Giải phóng USB reference */
		usb_put_dev(g_mon->udev);
		g_mon->udev        = NULL;
		g_mon->dev_present = false;

		pr_info("[rtl8188_mon] RTL8188ETV đã được rút ra\n");
		break;
	}

	return NOTIFY_OK;
}

/**
 * rtl8188_usb_init() - Đăng ký USB notifier và quét device đã có sẵn
 *
 * Gọi trong rtl8188_mon_init(). Sau khi đăng ký notifier, dùng
 * usb_for_each_dev() để phát hiện thiết bị đã cắm trước khi module load.
 */
void rtl8188_usb_init(void)
{
	g_mon->usb_nb.notifier_call = rtl8188_usb_notify;
	usb_register_notify(&g_mon->usb_nb);

	/*
	 * Quét tất cả USB device hiện có trên hệ thống.
	 * Xử lý trường hợp người dùng cắm USB trước khi insmod.
	 */
	usb_for_each_dev(NULL, find_our_usb);
}

/**
 * rtl8188_usb_exit() - Hủy đăng ký USB notifier
 *
 * Gọi trong rtl8188_mon_exit(). Sau khi unregister, không còn nhận
 * sự kiện plug/unplug nữa. Việc giải phóng g_mon->udev được xử lý
 * trong rtl8188_mon_exit() để đảm bảo thứ tự dọn dẹp đúng.
 */
void rtl8188_usb_exit(void)
{
	usb_unregister_notify(&g_mon->usb_nb);
}