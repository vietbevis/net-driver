// SPDX-License-Identifier: GPL-2.0
/*
 * RTL8188ETV USB WiFi driver - Hardware init and firmware upload
 *
 * Power-on sequence and firmware download derived from the
 * rtl8xxxu driver's RTL8188E support.
 */

#include <linux/module.h>
#include <linux/firmware.h>
#include <linux/delay.h>

#include "rtl8188.h"

/* --- Power-on sequence (RTL8188E specific) --- */

static int rtl8188_power_on(struct rtl8188_priv *priv)
{
	u16 val16;
	u32 val32;
	u8 val8;
	int count, ret;

	/* disabled -> emu: clear HW_SUSPEND and PCIE bits */
	ret = rtl8188_read16(priv, REG_APS_FSMCO, &val16);
	if (ret)
		return ret;
	val16 &= ~(APS_FSMCO_HW_SUSPEND | APS_FSMCO_PCIE);
	rtl8188_write16(priv, REG_APS_FSMCO, val16);

	/* Wait for power ready (bit 17 of APS_FSMCO) */
	for (count = RTL8188_MAX_REG_POLL; count > 0; count--) {
		ret = rtl8188_read32(priv, REG_APS_FSMCO, &val32);
		if (ret)
			return ret;
		if (val32 & BIT(17))
			break;
		udelay(10);
	}
	if (count == 0)
		return -ETIMEDOUT;

	/* Reset baseband */
	ret = rtl8188_read8(priv, REG_SYS_FUNC, &val8);
	if (ret)
		return ret;
	val8 &= ~(SYS_FUNC_BBRSTB | SYS_FUNC_BB_GLB_RSTN);
	rtl8188_write8(priv, REG_SYS_FUNC, val8);

	/* Schmit trigger on crystal */
	ret = rtl8188_read32(priv, REG_AFE_XTAL_CTRL, &val32);
	if (ret)
		return ret;
	val32 |= BIT(23);
	rtl8188_write32(priv, REG_AFE_XTAL_CTRL, val32);

	/* Disable HW powerdown */
	ret = rtl8188_read16(priv, REG_APS_FSMCO, &val16);
	if (ret)
		return ret;
	val16 &= ~APS_FSMCO_HW_POWERDOWN;
	rtl8188_write16(priv, REG_APS_FSMCO, val16);

	/* Disable WL suspend */
	ret = rtl8188_read16(priv, REG_APS_FSMCO, &val16);
	if (ret)
		return ret;
	val16 &= ~(APS_FSMCO_HW_SUSPEND | APS_FSMCO_PCIE);
	rtl8188_write16(priv, REG_APS_FSMCO, val16);

	/* emu -> active: Set MAC_ENABLE then poll until clear */
	ret = rtl8188_read32(priv, REG_APS_FSMCO, &val32);
	if (ret)
		return ret;
	val32 |= APS_FSMCO_MAC_ENABLE;
	rtl8188_write32(priv, REG_APS_FSMCO, val32);

	for (count = RTL8188_MAX_REG_POLL; count > 0; count--) {
		ret = rtl8188_read32(priv, REG_APS_FSMCO, &val32);
		if (ret)
			return ret;
		if (!(val32 & APS_FSMCO_MAC_ENABLE))
			break;
		udelay(10);
	}
	if (count == 0)
		return -ETIMEDOUT;

	/* LDO normal mode */
	ret = rtl8188_read8(priv, REG_LPLDO_CTRL, &val8);
	if (ret)
		return ret;
	val8 &= ~BIT(4);
	rtl8188_write8(priv, REG_LPLDO_CTRL, val8);

	/*
	 * Enable MAC DMA/WMAC/SCHEDULE/SEC block
	 * We do NOT set MAC_TX_ENABLE/MAC_RX_ENABLE yet -- the 8188E has a
	 * hardware bug requiring those to be set after REG_TRXFF_BNDY.
	 */
	val16 = CR_HCI_TXDMA_ENABLE | CR_HCI_RXDMA_ENABLE |
		CR_TXDMA_ENABLE | CR_RXDMA_ENABLE |
		CR_PROTOCOL_ENABLE | CR_SCHEDULE_ENABLE |
		CR_SECURITY_ENABLE | CR_CALTIMER_ENABLE;
	rtl8188_write16(priv, REG_CR, val16);

	return 0;
}

/* --- Auto LLT initialization --- */

static int rtl8188_auto_llt(struct rtl8188_priv *priv)
{
	u32 val32;
	int count;

	rtl8188_write32(priv, REG_AUTO_LLT, AUTO_LLT_INIT_LLT);

	for (count = RTL8188_MAX_REG_POLL; count > 0; count--) {
		rtl8188_read32(priv, REG_AUTO_LLT, &val32);
		if (!(val32 & AUTO_LLT_INIT_LLT))
			return 0;
		udelay(10);
	}
	return -ETIMEDOUT;
}

/* --- Page boundary and queue config --- */

static void rtl8188_init_queue_priority(struct rtl8188_priv *priv)
{
	/*
	 * RTL8188EU with 2 TX endpoints (EP2=high, EP3=normal):
	 *   total_pages=0x84, hq=0x11, lq=0x0e, nq=0x00
	 *   pubq = 0x84 - 0x11 - 0x0e - 0x00 - 1 = 0x64
	 */
	rtl8188_write32(priv, REG_RQPN,
			0x11 | (0x0e << 8) | (0x00 << 16) |
			(0x64 << 24) | RQPN_LOAD);
	rtl8188_write8(priv, REG_PBP,
		       (PBP_PAGE_SIZE_128 << 4) | PBP_PAGE_SIZE_128);
	/* TX buffer boundary */
	rtl8188_write8(priv, REG_TRXFF_BNDY, 0x00);
	/* RX buffer boundary at 0x25FF (critical for RX DMA to work) */
	rtl8188_write16(priv, REG_TRXFF_BNDY + 2, 0x25ff);
	/* RX DMA aggregate threshold: disabled */
	rtl8188_write32(priv, REG_RXDMA_AGG_PG_TH, 0);
}

/* --- Firmware download (RTL8188EU protocol) ---
 *
 * Sequence derived from rtl8xxxu: download_firmware + start_firmware
 */

static int rtl8188_download_firmware(struct rtl8188_priv *priv,
				     const u8 *fw_data, u32 fw_size)
{
	int pages, remainder, i, ret;
	u8 val8;
	u16 val16;
	u32 val32;
	const u8 *fwptr = fw_data;

	/* Ensure SYS_FUNC high byte has bit 2 set */
	rtl8188_read8(priv, REG_SYS_FUNC + 1, &val8);
	val8 |= 0x04;
	rtl8188_write8(priv, REG_SYS_FUNC + 1, val8);

	/* Enable 8051 CPU */
	rtl8188_read16(priv, REG_SYS_FUNC, &val16);
	val16 |= SYS_FUNC_CPU_ENABLE;
	rtl8188_write16(priv, REG_SYS_FUNC, val16);

	/* Check if firmware is already running */
	rtl8188_read8(priv, REG_MCU_FW_DL, &val8);
	if (val8 & MCU_FW_RAM_SEL) {
		dev_info(&priv->udev->dev,
			 "[RTL8188] Firmware already running, resetting MCU\n");
		rtl8188_write8(priv, REG_MCU_FW_DL, 0x00);
		/* Reset 8051 */
		rtl8188_read16(priv, REG_SYS_FUNC, &val16);
		val16 &= ~SYS_FUNC_CPU_ENABLE;
		rtl8188_write16(priv, REG_SYS_FUNC, val16);
		val16 |= SYS_FUNC_CPU_ENABLE;
		rtl8188_write16(priv, REG_SYS_FUNC, val16);
	}

	/* Enable MCU firmware download */
	rtl8188_read8(priv, REG_MCU_FW_DL, &val8);
	val8 |= MCU_FW_DL_ENABLE;
	rtl8188_write8(priv, REG_MCU_FW_DL, val8);

	/* 8051 reset via bit 19 of MCU_FW_DL */
	rtl8188_read32(priv, REG_MCU_FW_DL, &val32);
	val32 &= ~BIT(19);
	rtl8188_write32(priv, REG_MCU_FW_DL, val32);

	/* Reset firmware download checksum */
	rtl8188_read8(priv, REG_MCU_FW_DL, &val8);
	val8 |= MCU_FW_DL_CSUM_REPORT;
	rtl8188_write8(priv, REG_MCU_FW_DL, val8);

	/* Write firmware in page-sized chunks */
	pages = fw_size / RTL8188_FW_PAGE_SIZE;
	remainder = fw_size % RTL8188_FW_PAGE_SIZE;

	for (i = 0; i < pages; i++) {
		/* Set page number (byte 2 of MCU_FW_DL, bits 0-2) */
		rtl8188_read8(priv, REG_MCU_FW_DL + 2, &val8);
		val8 = (val8 & 0xf8) | i;
		rtl8188_write8(priv, REG_MCU_FW_DL + 2, val8);

		ret = rtl8188_write_block(priv, REG_FW_START_ADDRESS,
					  fwptr, RTL8188_FW_PAGE_SIZE);
		if (ret) {
			dev_err(&priv->udev->dev,
				"[RTL8188] FW write failed page %d\n", i);
			return ret;
		}
		fwptr += RTL8188_FW_PAGE_SIZE;
	}

	if (remainder) {
		rtl8188_read8(priv, REG_MCU_FW_DL + 2, &val8);
		val8 = (val8 & 0xf8) | i;
		rtl8188_write8(priv, REG_MCU_FW_DL + 2, val8);

		ret = rtl8188_write_block(priv, REG_FW_START_ADDRESS,
					  fwptr, remainder);
		if (ret) {
			dev_err(&priv->udev->dev,
				"[RTL8188] FW write failed remainder\n");
			return ret;
		}
	}

	/* Disable MCU firmware download */
	rtl8188_read16(priv, REG_MCU_FW_DL, &val16);
	val16 &= ~MCU_FW_DL_ENABLE;
	rtl8188_write16(priv, REG_MCU_FW_DL, val16);

	return 0;
}

static int rtl8188_start_firmware(struct rtl8188_priv *priv)
{
	u32 val32;
	u16 val16;
	int i;

	/* Poll for checksum report */
	for (i = 0; i < 1000; i++) {
		rtl8188_read32(priv, REG_MCU_FW_DL, &val32);
		if (val32 & MCU_FW_DL_CSUM_REPORT)
			break;
	}
	if (i == 1000) {
		dev_warn(&priv->udev->dev,
			 "[RTL8188] Firmware checksum poll timed out\n");
		return -EAGAIN;
	}

	/* Set FW_DL_READY and clear WINT_INIT_READY */
	rtl8188_read32(priv, REG_MCU_FW_DL, &val32);
	val32 |= MCU_FW_DL_READY;
	val32 &= ~MCU_WINT_INIT_READY;
	rtl8188_write32(priv, REG_MCU_FW_DL, val32);

	/* Reset 8051 to start firmware */
	rtl8188_read16(priv, REG_SYS_FUNC, &val16);
	val16 &= ~SYS_FUNC_CPU_ENABLE;
	rtl8188_write16(priv, REG_SYS_FUNC, val16);
	val16 |= SYS_FUNC_CPU_ENABLE;
	rtl8188_write16(priv, REG_SYS_FUNC, val16);

	/* Wait for firmware to become ready */
	for (i = 0; i < 1000; i++) {
		rtl8188_read32(priv, REG_MCU_FW_DL, &val32);
		if (val32 & MCU_WINT_INIT_READY) {
			dev_info(&priv->udev->dev,
				 "[RTL8188] Firmware ready\n");
			return 0;
		}
		udelay(100);
	}

	dev_err(&priv->udev->dev, "[RTL8188] Firmware not ready (timeout)\n");
	return -ETIMEDOUT;
}

static int rtl8188_load_firmware(struct rtl8188_priv *priv)
{
	const u8 *fw_data;
	u32 fw_size;
	int ret;

	ret = request_firmware(&priv->fw, RTL8188_FW_NAME, &priv->udev->dev);
	if (ret) {
		dev_err(&priv->udev->dev,
			"[RTL8188] Firmware %s not found (%d)\n",
			RTL8188_FW_NAME, ret);
		return ret;
	}

	if (priv->fw->size <= RTL8188_FW_HDR_SIZE) {
		dev_err(&priv->udev->dev, "[RTL8188] Firmware too small\n");
		release_firmware(priv->fw);
		priv->fw = NULL;
		return -EINVAL;
	}

	/* Skip 32-byte firmware header */
	fw_data = priv->fw->data + RTL8188_FW_HDR_SIZE;
	fw_size = priv->fw->size - RTL8188_FW_HDR_SIZE;

	dev_info(&priv->udev->dev,
		 "[RTL8188] Firmware loaded (%zu bytes, payload %u bytes)\n",
		 priv->fw->size, fw_size);

	ret = rtl8188_download_firmware(priv, fw_data, fw_size);
	if (ret)
		return ret;

	return rtl8188_start_firmware(priv);
}

/* --- EFUSE reading --- */

static int rtl8188_read_efuse8(struct rtl8188_priv *priv, u16 offset, u8 *data)
{
	u8 val8;
	u32 val32;
	int i;

	rtl8188_write8(priv, REG_EFUSE_CTRL + 1, offset & 0xff);
	rtl8188_read8(priv, REG_EFUSE_CTRL + 2, &val8);
	val8 = (val8 & 0xfc) | ((offset >> 8) & 0x03);
	rtl8188_write8(priv, REG_EFUSE_CTRL + 2, val8);

	rtl8188_read8(priv, REG_EFUSE_CTRL + 3, &val8);
	rtl8188_write8(priv, REG_EFUSE_CTRL + 3, val8 & 0x7f);

	for (i = 0; i < RTL8188_MAX_REG_POLL; i++) {
		rtl8188_read32(priv, REG_EFUSE_CTRL, &val32);
		if (val32 & BIT(31))
			break;
	}
	if (i == RTL8188_MAX_REG_POLL)
		return -EIO;

	udelay(50);
	rtl8188_read32(priv, REG_EFUSE_CTRL, &val32);
	*data = val32 & 0xff;
	return 0;
}

static int rtl8188_read_efuse(struct rtl8188_priv *priv)
{
	int i, ret;
	u8 header, extheader, word_mask, val8;
	u16 efuse_addr, offset, map_addr;

	rtl8188_write8(priv, REG_EFUSE_ACCESS, EFUSE_ACCESS_ENABLE);

	memset(priv->efuse_map, 0xff, EFUSE_MAP_LEN);

	efuse_addr = 0;
	while (efuse_addr < EFUSE_REAL_CONTENT_LEN) {
		ret = rtl8188_read_efuse8(priv, efuse_addr++, &header);
		if (ret || header == 0xff)
			goto exit;

		if ((header & 0x1f) == 0x0f) {
			offset = (header & 0xe0) >> 5;
			ret = rtl8188_read_efuse8(priv, efuse_addr++,
						  &extheader);
			if (ret)
				goto exit;
			if ((extheader & 0x0f) == 0x0f)
				continue;
			offset |= ((extheader & 0xf0) >> 1);
			word_mask = extheader & 0x0f;
		} else {
			offset = (header >> 4) & 0x0f;
			word_mask = header & 0x0f;
		}

		map_addr = offset * 8;
		for (i = 0; i < EFUSE_MAX_WORD_UNIT; i++) {
			if (word_mask & BIT(i)) {
				map_addr += 2;
				continue;
			}
			ret = rtl8188_read_efuse8(priv, efuse_addr++, &val8);
			if (ret)
				goto exit;
			if (map_addr >= EFUSE_MAP_LEN - 1)
				goto exit;
			priv->efuse_map[map_addr++] = val8;

			ret = rtl8188_read_efuse8(priv, efuse_addr++, &val8);
			if (ret)
				goto exit;
			priv->efuse_map[map_addr++] = val8;
		}
	}

exit:
	rtl8188_write8(priv, REG_EFUSE_ACCESS, EFUSE_ACCESS_DISABLE);
	return 0;
}

/* --- Read MAC address from EFUSE --- */

int rtl8188_read_mac_addr(struct rtl8188_priv *priv)
{
	int ret;

	ret = rtl8188_read_efuse(priv);
	if (ret)
		return ret;

	memcpy(priv->mac_addr, &priv->efuse_map[EFUSE_MAC_ADDR_OFFSET],
	       ETH_ALEN);

	if (is_zero_ether_addr(priv->mac_addr) ||
	    is_multicast_ether_addr(priv->mac_addr)) {
		eth_random_addr(priv->mac_addr);
		dev_warn(&priv->udev->dev,
			 "[RTL8188] Invalid EFUSE MAC, using random: %pM\n",
			 priv->mac_addr);
	}

	return 0;
}

/* --- Post-init configuration --- */

static void rtl8188_hw_post_init(struct rtl8188_priv *priv)
{
	u16 val16;

	/*
	 * 8188E hardware bug: MAC_TX/RX_ENABLE must be set AFTER
	 * REG_TRXFF_BNDY is configured.
	 */
	rtl8188_read16(priv, REG_CR, &val16);
	val16 |= CR_MAC_TX_ENABLE | CR_MAC_RX_ENABLE;
	rtl8188_write16(priv, REG_CR, val16);

	/* Disable RX DMA aggregation so packets arrive immediately */
	{
		u8 val;

		rtl8188_read8(priv, REG_TRXDMA_CTRL, &val);
		val &= ~REG_TRXDMA_CTRL_RXDMA_AGG_EN;
		rtl8188_write8(priv, REG_TRXDMA_CTRL, val);
	}
	rtl8188_write32(priv, REG_RXDMA_AGG_PG_TH, 0);

	/* Clear RX packet count */
	rtl8188_write16(priv, REG_RXPKT_NUM, 0);

	/* Pre-TX enable WEP/TKIP security */
	rtl8188_write8(priv, REG_EARLY_MODE_CONTROL_8188E + 3, 0x01);

	/* TX report control for rate adaptation */
	rtl8188_write8(priv, REG_TX_REPORT_CTRL,
		       TX_REPORT_CTRL_TIMER_ENABLE);
	rtl8188_write16(priv, REG_TX_REPORT_TIME, 0x927c);

	/* SIFS timings */
	rtl8188_write16(priv, REG_SPEC_SIFS, 0x100a);
	rtl8188_write16(priv, REG_MAC_SPEC_SIFS, 0x100a);
	rtl8188_write16(priv, REG_SIFS_CCK, 0x100a);
	rtl8188_write16(priv, REG_SIFS_OFDM, 0x100a);

	/* Retry limits */
	rtl8188_write16(priv, REG_RETRY_LIMIT, 0x3030);

	/* EDCA parameters (defaults) */
	rtl8188_write32(priv, REG_EDCA_VO_PARAM, 0x002fa226);
	rtl8188_write32(priv, REG_EDCA_VI_PARAM, 0x005ea324);
	rtl8188_write32(priv, REG_EDCA_BE_PARAM, 0x005ea42b);
	rtl8188_write32(priv, REG_EDCA_BK_PARAM, 0x0000a44f);

	/* Slot time */
	rtl8188_write8(priv, REG_SLOT, 0x09);

	/* USTIME_EDCA */
	rtl8188_write8(priv, REG_USTIME_EDCA, 0x28);

	/* ACK timeout */
	rtl8188_write8(priv, REG_ACKTO, 0x40);

	/* Response rate set */
	rtl8188_write32(priv, REG_RESPONSE_RATE_SET, 0x0000015f);

	/* Bandwidth 20MHz by default */
	rtl8188_write8(priv, REG_BW_OPMODE, BW_OPMODE_20MHZ);

	/* Security config: no default key, no hw encryption */
	rtl8188_write8(priv, REG_SECURITY_CFG, 0x00);

	/* Accept management and data filter maps */
	rtl8188_write16(priv, REG_RXFLTMAP0, 0xffff);
	rtl8188_write16(priv, REG_RXFLTMAP2, 0xffff);

	/* BCN interval */
	rtl8188_write16(priv, REG_BCN_INTERVAL, 100);
}

/* --- Top-level hardware init --- */

int rtl8188_hw_init(struct rtl8188_priv *priv)
{
	int ret;

	ret = rtl8188_power_on(priv);
	if (ret) {
		dev_err(&priv->udev->dev,
			"[RTL8188] Power-on failed: %d\n", ret);
		return ret;
	}

	rtl8188_init_queue_priority(priv);

	ret = rtl8188_auto_llt(priv);
	if (ret) {
		dev_err(&priv->udev->dev,
			"[RTL8188] Auto-LLT failed: %d\n", ret);
		return ret;
	}

	ret = rtl8188_load_firmware(priv);
	if (ret)
		return ret;

	ret = rtl8188_read_mac_addr(priv);
	if (ret)
		return ret;

	rtl8188_hw_post_init(priv);

	dev_info(&priv->udev->dev, "[RTL8188] Hardware initialized\n");
	return 0;
}
