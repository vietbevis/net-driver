// SPDX-License-Identifier: GPL-2.0
/*
 * RTL8188ETV USB WiFi driver - TX/RX data path
 */

#include <linux/usb.h>
#include <linux/skbuff.h>
#include <net/mac80211.h>
#include "rtl8188.h"

/* --- TX path --- */

static void rtl8188_tx_complete(struct urb *urb)
{
	struct rtl8188_tx_urb *tx_urb = urb->context;
	struct ieee80211_hw *hw = tx_urb->hw;
	struct sk_buff *skb = tx_urb->skb;
	struct ieee80211_tx_info *info;

	if (skb) {
		info = IEEE80211_SKB_CB(skb);
		ieee80211_tx_info_clear_status(info);

		if (urb->status == 0)
			info->flags |= IEEE80211_TX_STAT_ACK;

		ieee80211_tx_status_irqsafe(hw, skb);
	}

	kfree(tx_urb);
}

void rtl8188_tx_submit(struct ieee80211_hw *hw, struct sk_buff *skb)
{
	struct rtl8188_priv *priv = hw->priv;
	struct rtl8188_tx_urb *tx_urb;
	struct rtl8188_tx_desc *desc;
	struct ieee80211_hdr *hdr;
	struct ieee80211_tx_info *info;
	struct ieee80211_tx_rate *rate;
	u8 *buf;
	int buf_len, ret;
	bool is_mgmt;
	u8 queue_sel;
	u8 hw_rate;
	unsigned int pipe;

	info = IEEE80211_SKB_CB(skb);
	hdr = (struct ieee80211_hdr *)skb->data;
	is_mgmt = ieee80211_is_mgmt(hdr->frame_control);

	/* Pick TX rate: use first rate from rate control or default 1Mbps */
	rate = &info->control.rates[0];
	if (rate->idx < 0 || (rate->flags & IEEE80211_TX_RC_MCS))
		hw_rate = 0; /* 1 Mbps CCK */
	else
		hw_rate = rate->idx;

	/* Allocate TX URB + buffer: descriptor + frame */
	buf_len = RTL8188_TX_DESC_SIZE + skb->len;
	buf = kzalloc(buf_len, GFP_ATOMIC);
	if (!buf) {
		ieee80211_free_txskb(hw, skb);
		return;
	}

	tx_urb = kzalloc(sizeof(*tx_urb), GFP_ATOMIC);
	if (!tx_urb) {
		kfree(buf);
		ieee80211_free_txskb(hw, skb);
		return;
	}

	desc = (struct rtl8188_tx_desc *)buf;

	/* pkt_size: 802.11 frame length (not counting TX descriptor) */
	desc->pkt_size = cpu_to_le16(skb->len);

	/* pkt_offset: extra bytes (in 8-byte units) between fixed desc and
	 * payload. We have NO extra bytes, so this must be 0. */
	desc->pkt_offset = 0;

	/* txdw0 (byte 3): OWN | FIRST_SEG | LAST_SEG, BMC if broadcast/mcast */
	desc->txdw0 = TXDESC_OWN | TXDESC_FIRST_SEGMENT | TXDESC_LAST_SEGMENT;
	if (is_multicast_ether_addr(hdr->addr1))
		desc->txdw0 |= TXDESC_BROADMULTICAST;

	/* txdw1: queue select in bits [11:8]
	 * MGNT queue (0x12) for management, BE (0x0) for data */
	queue_sel = is_mgmt ? TXDESC_QUEUE_MGNT : TXDESC_QUEUE_BE;
	desc->txdw1 = cpu_to_le32((u32)queue_sel << TXDESC_QUEUE_SHIFT);

	/* txdw3: seq number for data frames */
	if (!is_mgmt) {
		u16 seq = le16_to_cpu(hdr->seq_ctrl) >> 4;

		desc->txdw3 = cpu_to_le32((u32)seq << TXDESC32_SEQ_SHIFT);
	}

	/* txdw4: rate control
	 * Use driver-supplied rate for management frames; HW seq enable */
	desc->txdw4 = cpu_to_le32(TXDESC32_USE_DRIVER_RATE |
				   TXDESC32_HW_SEQ_ENABLE |
				   ((u32)hw_rate & 0x3f));

	/* txdw5: retry limit + rate fallback */
	desc->txdw5 = cpu_to_le32(TXDESC32_RETRY_LIMIT_ENABLE |
				   (0x30 << 8) | /* max 48 retries */
				   ((u32)hw_rate & 0x3f));

	/* Copy 802.11 frame immediately after the 32-byte TX descriptor */
	memcpy(buf + RTL8188_TX_DESC_SIZE, skb->data, skb->len);

	tx_urb->hw = hw;
	tx_urb->skb = skb;

	/* Route to correct endpoint: mgmt/VI/VO -> EP2, BE/BK -> EP3 */
	pipe = is_mgmt ? priv->pipe_out : priv->pipe_out_be;

	usb_init_urb(&tx_urb->urb);
	usb_fill_bulk_urb(&tx_urb->urb, priv->udev, pipe,
			  buf, buf_len, rtl8188_tx_complete, tx_urb);
	tx_urb->urb.transfer_flags |= URB_FREE_BUFFER;

	ret = usb_submit_urb(&tx_urb->urb, GFP_ATOMIC);
	if (ret) {
		dev_warn(&priv->udev->dev,
			 "[RTL8188] TX URB submit failed: %d\n", ret);
		kfree(tx_urb);
		kfree(buf);
		ieee80211_free_txskb(hw, skb);
	}
}

/* --- RX path --- */

static void rtl8188_rx_complete(struct urb *urb);

static int rtl8188_submit_rx_urb(struct rtl8188_priv *priv,
				 struct rtl8188_rx_urb *rx_urb)
{
	usb_fill_bulk_urb(&rx_urb->urb, priv->udev, priv->pipe_in,
			  rx_urb->buf, RTL8188_RX_BUF_SIZE,
			  rtl8188_rx_complete, rx_urb);

	return usb_submit_urb(&rx_urb->urb, GFP_ATOMIC);
}

static void rtl8188_rx_process(struct rtl8188_rx_urb *rx_urb,
			       int actual_length)
{
	struct ieee80211_hw *hw = rx_urb->hw;
	u8 *buf = rx_urb->buf;
	int offset = 0;

	while (offset + RTL8188_RX_DESC_SIZE < actual_length) {
		u32 rxdw0, rxdw3;
		u16 pkt_len;
		u8 drvinfo_sz;
		u8 shift;
		struct sk_buff *skb;
		struct ieee80211_rx_status *rx_status;
		int frame_offset;
		int total_pkt_len;

		rxdw0 = le32_to_cpup((__le32 *)(buf + offset));
		rxdw3 = le32_to_cpup((__le32 *)(buf + offset + 12));

		pkt_len = rxdw0 & RXDESC_PKT_LEN_MASK;
		drvinfo_sz = ((rxdw0 & RXDESC_DRVINFO_SZ_MASK) >>
			      RXDESC_DRVINFO_SZ_SHIFT) * 8;
		shift = (rxdw0 >> 24) & 0x03;

		if (pkt_len == 0 || pkt_len > 8192)
			break;

		frame_offset = RTL8188_RX_DESC_SIZE + drvinfo_sz + shift;
		total_pkt_len = frame_offset + pkt_len;

		if (offset + total_pkt_len > actual_length)
			break;

		/* Skip CRC error frames */
		if (rxdw0 & RXDESC_CRC32)
			goto next;

		skb = dev_alloc_skb(pkt_len);
		if (!skb)
			goto next;

		skb_put_data(skb, buf + offset + frame_offset, pkt_len);

		rx_status = IEEE80211_SKB_RXCB(skb);
		memset(rx_status, 0, sizeof(*rx_status));

		rx_status->freq = hw->conf.chandef.chan ?
			hw->conf.chandef.chan->center_freq : 2412;
		rx_status->band = NL80211_BAND_2GHZ;

		/* Signal quality from PHY stats (if present) */
		if ((rxdw0 & RXDESC_PHY_STATS) && drvinfo_sz >= 4) {
			u8 *phystats = buf + offset + RTL8188_RX_DESC_SIZE;
			s8 pwdb = (s8)phystats[1];

			rx_status->signal = pwdb;
		} else {
			rx_status->signal = -50;
		}

		/* Rate info */
		if (rxdw3 & RXDESC_RXHT) {
			rx_status->encoding = RX_ENC_HT;
			rx_status->rate_idx = rxdw3 & 0x1f;
		} else {
			rx_status->rate_idx = rxdw3 & RXDESC_RXMCS_MASK;
			/* Map HW rate index to mac80211 rate index */
			if (rx_status->rate_idx >= 4)
				rx_status->rate_idx -= 4;
			if (rx_status->rate_idx >= 12)
				rx_status->rate_idx = 0;
		}

		rx_status->flag |= RX_FLAG_MACTIME_START;

		((struct rtl8188_priv *)hw->priv)->rx_frame_count++;
		ieee80211_rx_irqsafe(hw, skb);

next:
		/* Align to 128-byte boundary for next packet */
		total_pkt_len = ALIGN(total_pkt_len, 128);
		offset += total_pkt_len;
	}
}

static void rtl8188_rx_complete(struct urb *urb)
{
	struct rtl8188_rx_urb *rx_urb = urb->context;
	struct rtl8188_priv *priv = rx_urb->hw->priv;

	/* Log first 10 completions regardless of content */
	if (priv->rx_urb_count + priv->rx_err_count < 10) {
		dev_info(&priv->udev->dev,
			 "[RTL8188] RX completion: status=%d len=%d\n",
			 urb->status, urb->actual_length);
	}

	if (urb->status == 0 && urb->actual_length > 0) {
		priv->rx_urb_count++;
		priv->rx_byte_count += urb->actual_length;
		rtl8188_rx_process(rx_urb, urb->actual_length);
	} else if (urb->status != 0 && urb->status != -ENOENT &&
		   urb->status != -ECONNRESET && urb->status != -ESHUTDOWN) {
		priv->rx_err_count++;
		dev_warn(&priv->udev->dev,
			 "[RTL8188] RX URB error: status=%d\n", urb->status);
	}

	if (!priv->shutdown && urb->status != -ENOENT &&
	    urb->status != -ECONNRESET && urb->status != -ESHUTDOWN)
		rtl8188_submit_rx_urb(priv, rx_urb);
}

int rtl8188_rx_start(struct rtl8188_priv *priv)
{
	int i, ret;

	priv->shutdown = false;

	for (i = 0; i < RTL8188_NUM_RX_URBS; i++) {
		struct rtl8188_rx_urb *rx_urb = &priv->rx_urbs[i];

		rx_urb->hw = priv->hw;

		if (!rx_urb->buf) {
			rx_urb->buf = kmalloc(RTL8188_RX_BUF_SIZE, GFP_KERNEL);
			if (!rx_urb->buf)
				return -ENOMEM;
		}

		usb_init_urb(&rx_urb->urb);
		ret = rtl8188_submit_rx_urb(priv, rx_urb);
		if (ret) {
			dev_err(&priv->udev->dev,
				"[RTL8188] RX URB %d submit failed: %d\n",
				i, ret);
			return ret;
		}
	}

	return 0;
}

void rtl8188_rx_stop(struct rtl8188_priv *priv)
{
	int i;

	priv->shutdown = true;

	for (i = 0; i < RTL8188_NUM_RX_URBS; i++) {
		usb_kill_urb(&priv->rx_urbs[i].urb);
		kfree(priv->rx_urbs[i].buf);
		priv->rx_urbs[i].buf = NULL;
	}
}
