// SPDX-License-Identifier: GPL-2.0
/*
 * RTL8188ETV USB WiFi driver - USB driver core
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/usb.h>
#include <linux/slab.h>
#include <linux/firmware.h>
#include <net/mac80211.h>

#include "rtl8188.h"

static const struct usb_device_id rtl8188_device_table[] = {
	{ USB_DEVICE(RTL8188_USB_VENDOR_ID, RTL8188_USB_PRODUCT_ID) },
	{}
};
MODULE_DEVICE_TABLE(usb, rtl8188_device_table);

/* --- USB register I/O via vendor control transfers --- */

int rtl8188_read8(struct rtl8188_priv *priv, u16 addr, u8 *val)
{
	int ret;

	mutex_lock(&priv->io_mutex);
	ret = usb_control_msg(priv->udev, usb_rcvctrlpipe(priv->udev, 0),
			      RTL8188_USB_CMD_REQ, RTL8188_USB_CMD_READ,
			      addr, 0, priv->io_buf, 1,
			      RTL8188_USB_CMD_TIMEOUT);
	if (ret == 1) {
		*val = priv->io_buf[0];
		ret = 0;
	} else if (ret >= 0) {
		ret = -EIO;
	}
	mutex_unlock(&priv->io_mutex);
	return ret;
}

int rtl8188_read16(struct rtl8188_priv *priv, u16 addr, u16 *val)
{
	int ret;

	mutex_lock(&priv->io_mutex);
	ret = usb_control_msg(priv->udev, usb_rcvctrlpipe(priv->udev, 0),
			      RTL8188_USB_CMD_REQ, RTL8188_USB_CMD_READ,
			      addr, 0, priv->io_buf, 2,
			      RTL8188_USB_CMD_TIMEOUT);
	if (ret == 2) {
		*val = le16_to_cpup((__le16 *)priv->io_buf);
		ret = 0;
	} else if (ret >= 0) {
		ret = -EIO;
	}
	mutex_unlock(&priv->io_mutex);
	return ret;
}

int rtl8188_read32(struct rtl8188_priv *priv, u16 addr, u32 *val)
{
	int ret;

	mutex_lock(&priv->io_mutex);
	ret = usb_control_msg(priv->udev, usb_rcvctrlpipe(priv->udev, 0),
			      RTL8188_USB_CMD_REQ, RTL8188_USB_CMD_READ,
			      addr, 0, priv->io_buf, 4,
			      RTL8188_USB_CMD_TIMEOUT);
	if (ret == 4) {
		*val = le32_to_cpup((__le32 *)priv->io_buf);
		ret = 0;
	} else if (ret >= 0) {
		ret = -EIO;
	}
	mutex_unlock(&priv->io_mutex);
	return ret;
}

int rtl8188_write8(struct rtl8188_priv *priv, u16 addr, u8 val)
{
	int ret;

	mutex_lock(&priv->io_mutex);
	priv->io_buf[0] = val;
	ret = usb_control_msg(priv->udev, usb_sndctrlpipe(priv->udev, 0),
			      RTL8188_USB_CMD_REQ, RTL8188_USB_CMD_WRITE,
			      addr, 0, priv->io_buf, 1,
			      RTL8188_USB_CMD_TIMEOUT);
	mutex_unlock(&priv->io_mutex);
	return (ret == 1) ? 0 : (ret < 0 ? ret : -EIO);
}

int rtl8188_write16(struct rtl8188_priv *priv, u16 addr, u16 val)
{
	int ret;

	mutex_lock(&priv->io_mutex);
	put_unaligned_le16(val, priv->io_buf);
	ret = usb_control_msg(priv->udev, usb_sndctrlpipe(priv->udev, 0),
			      RTL8188_USB_CMD_REQ, RTL8188_USB_CMD_WRITE,
			      addr, 0, priv->io_buf, 2,
			      RTL8188_USB_CMD_TIMEOUT);
	mutex_unlock(&priv->io_mutex);
	return (ret == 2) ? 0 : (ret < 0 ? ret : -EIO);
}

int rtl8188_write32(struct rtl8188_priv *priv, u16 addr, u32 val)
{
	int ret;

	mutex_lock(&priv->io_mutex);
	put_unaligned_le32(val, priv->io_buf);
	ret = usb_control_msg(priv->udev, usb_sndctrlpipe(priv->udev, 0),
			      RTL8188_USB_CMD_REQ, RTL8188_USB_CMD_WRITE,
			      addr, 0, priv->io_buf, 4,
			      RTL8188_USB_CMD_TIMEOUT);
	mutex_unlock(&priv->io_mutex);
	return (ret == 4) ? 0 : (ret < 0 ? ret : -EIO);
}

int rtl8188_write_block(struct rtl8188_priv *priv, u16 addr,
			const u8 *buf, u16 len)
{
	int ret;
	u16 off = 0;

	while (off < len) {
		u16 chunk = min_t(u16, len - off, 64);

		mutex_lock(&priv->io_mutex);
		memcpy(priv->io_buf, buf + off, chunk);
		ret = usb_control_msg(priv->udev,
				      usb_sndctrlpipe(priv->udev, 0),
				      RTL8188_USB_CMD_REQ,
				      RTL8188_USB_CMD_WRITE,
				      addr + off, 0,
				      priv->io_buf, chunk,
				      RTL8188_USB_CMD_TIMEOUT);
		mutex_unlock(&priv->io_mutex);

		if (ret != chunk)
			return (ret < 0) ? ret : -EIO;
		off += chunk;
	}
	return 0;
}

/* --- USB endpoint discovery --- */

static int rtl8188_find_endpoints(struct rtl8188_priv *priv)
{
	struct usb_interface *intf = priv->intf;
	struct usb_host_interface *alt = intf->cur_altsetting;
	struct usb_endpoint_descriptor *ep;
	int i, out_count = 0;
	bool found_in = false;

	for (i = 0; i < alt->desc.bNumEndpoints; i++) {
		ep = &alt->endpoint[i].desc;

		if (usb_endpoint_is_bulk_in(ep) && !found_in) {
			priv->pipe_in = usb_rcvbulkpipe(priv->udev,
						ep->bEndpointAddress);
			found_in = true;
		} else if (usb_endpoint_is_bulk_out(ep)) {
			if (out_count == 0) {
				/* EP2: high priority (mgmt/VI/VO) */
				priv->pipe_out = usb_sndbulkpipe(priv->udev,
						ep->bEndpointAddress);
			} else if (out_count == 1) {
				/* EP3: normal priority (BE/BK) */
				priv->pipe_out_be = usb_sndbulkpipe(priv->udev,
						ep->bEndpointAddress);
			}
			out_count++;
		}
	}

	/* Fall back: if only one OUT EP, use it for both */
	if (out_count == 1)
		priv->pipe_out_be = priv->pipe_out;

	if (!found_in || out_count == 0) {
		dev_err(&intf->dev, "Missing bulk endpoints\n");
		return -ENODEV;
	}

	dev_info(&intf->dev, "[RTL8188] Endpoints: IN=%d OUT=%d OUT_BE=%d\n",
		 found_in, priv->pipe_out, priv->pipe_out_be);

	return 0;
}

/* --- Probe / Disconnect --- */

static int rtl8188_probe(struct usb_interface *intf,
			 const struct usb_device_id *id)
{
	struct ieee80211_hw *hw;
	struct rtl8188_priv *priv;
	int ret;

	hw = ieee80211_alloc_hw(sizeof(*priv), &rtl8188_ops);
	if (!hw)
		return -ENOMEM;

	priv = hw->priv;
	priv->hw = hw;
	priv->udev = usb_get_dev(interface_to_usbdev(intf));
	priv->intf = intf;
	mutex_init(&priv->io_mutex);

	priv->io_buf = kmalloc(256, GFP_KERNEL);
	if (!priv->io_buf) {
		ret = -ENOMEM;
		goto err_free_hw;
	}

	usb_set_intfdata(intf, hw);

	/* Disable USB autosuspend */
	usb_disable_autosuspend(priv->udev);

	ret = rtl8188_find_endpoints(priv);
	if (ret)
		goto err_free_buf;

	dev_info(&intf->dev, "[RTL8188] Device detected: %04x:%04x\n",
		 id->idVendor, id->idProduct);

	ret = rtl8188_hw_init(priv);
	if (ret) {
		dev_err(&intf->dev, "[RTL8188] Hardware init failed: %d\n", ret);
		goto err_free_buf;
	}

	ret = rtl8188_phy_init(priv);
	if (ret) {
		dev_err(&intf->dev, "[RTL8188] PHY init failed: %d\n", ret);
		goto err_release_fw;
	}

	ret = rtl8188_mac_setup(priv);
	if (ret) {
		dev_err(&intf->dev, "[RTL8188] mac80211 setup failed: %d\n", ret);
		goto err_release_fw;
	}

	SET_IEEE80211_DEV(hw, &intf->dev);

	ret = ieee80211_register_hw(hw);
	if (ret) {
		dev_err(&intf->dev, "[RTL8188] register_hw failed: %d\n", ret);
		goto err_release_fw;
	}

	dev_info(&intf->dev, "[RTL8188] Driver loaded, MAC: %pM\n",
		 priv->mac_addr);
	return 0;

err_release_fw:
	if (priv->fw) {
		release_firmware(priv->fw);
		priv->fw = NULL;
	}
err_free_buf:
	kfree(priv->io_buf);
err_free_hw:
	usb_put_dev(priv->udev);
	usb_set_intfdata(intf, NULL);
	ieee80211_free_hw(hw);
	return ret;
}

static void rtl8188_disconnect(struct usb_interface *intf)
{
	struct ieee80211_hw *hw = usb_get_intfdata(intf);
	struct rtl8188_priv *priv;

	if (!hw)
		return;

	priv = hw->priv;
	priv->shutdown = true;

	rtl8188_rx_stop(priv);

	ieee80211_unregister_hw(hw);

	if (priv->fw) {
		release_firmware(priv->fw);
		priv->fw = NULL;
	}

	kfree(priv->io_buf);
	usb_put_dev(priv->udev);
	usb_set_intfdata(intf, NULL);

	ieee80211_free_hw(hw);

	dev_info(&intf->dev, "[RTL8188] Device disconnected\n");
}

static struct usb_driver rtl8188_usb_driver = {
	.name		= "rtl8188_drv",
	.probe		= rtl8188_probe,
	.disconnect	= rtl8188_disconnect,
	.id_table	= rtl8188_device_table,
	.soft_unbind	= 1,
};

module_usb_driver(rtl8188_usb_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Custom Driver Project");
MODULE_DESCRIPTION("RTL8188ETV USB WiFi mac80211 Driver");
MODULE_FIRMWARE(RTL8188_FW_NAME);
