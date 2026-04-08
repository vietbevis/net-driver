// SPDX-License-Identifier: GPL-2.0
/*
 * RTL8188ETV USB WiFi driver - mac80211 integration
 */

#include <linux/etherdevice.h>
#include <net/mac80211.h>
#include "rtl8188.h"

/* --- 2.4 GHz channel definitions --- */

static const struct ieee80211_channel rtl8188_channels_2ghz[] = {
	{ .band = NL80211_BAND_2GHZ, .center_freq = 2412, .hw_value = 1 },
	{ .band = NL80211_BAND_2GHZ, .center_freq = 2417, .hw_value = 2 },
	{ .band = NL80211_BAND_2GHZ, .center_freq = 2422, .hw_value = 3 },
	{ .band = NL80211_BAND_2GHZ, .center_freq = 2427, .hw_value = 4 },
	{ .band = NL80211_BAND_2GHZ, .center_freq = 2432, .hw_value = 5 },
	{ .band = NL80211_BAND_2GHZ, .center_freq = 2437, .hw_value = 6 },
	{ .band = NL80211_BAND_2GHZ, .center_freq = 2442, .hw_value = 7 },
	{ .band = NL80211_BAND_2GHZ, .center_freq = 2447, .hw_value = 8 },
	{ .band = NL80211_BAND_2GHZ, .center_freq = 2452, .hw_value = 9 },
	{ .band = NL80211_BAND_2GHZ, .center_freq = 2457, .hw_value = 10 },
	{ .band = NL80211_BAND_2GHZ, .center_freq = 2462, .hw_value = 11 },
	{ .band = NL80211_BAND_2GHZ, .center_freq = 2467, .hw_value = 12 },
	{ .band = NL80211_BAND_2GHZ, .center_freq = 2472, .hw_value = 13 },
	{ .band = NL80211_BAND_2GHZ, .center_freq = 2484, .hw_value = 14 },
};

/* --- Rate definitions: 4 CCK + 8 OFDM --- */

static const struct ieee80211_rate rtl8188_rates_2ghz[] = {
	{ .bitrate = 10,  .hw_value = 0x00 },
	{ .bitrate = 20,  .hw_value = 0x01 },
	{ .bitrate = 55,  .hw_value = 0x02 },
	{ .bitrate = 110, .hw_value = 0x03 },
	{ .bitrate = 60,  .hw_value = 0x04 },
	{ .bitrate = 90,  .hw_value = 0x05 },
	{ .bitrate = 120, .hw_value = 0x06 },
	{ .bitrate = 180, .hw_value = 0x07 },
	{ .bitrate = 240, .hw_value = 0x08 },
	{ .bitrate = 360, .hw_value = 0x09 },
	{ .bitrate = 480, .hw_value = 0x0a },
	{ .bitrate = 540, .hw_value = 0x0b },
};

/* --- HT (802.11n) MCS 0-7 --- */

static const struct ieee80211_sta_ht_cap rtl8188_ht_cap = {
	.ht_supported = true,
	.cap = IEEE80211_HT_CAP_SGI_20 |
	       IEEE80211_HT_CAP_DSSSCCK40 |
	       IEEE80211_HT_CAP_MAX_AMSDU,
	.ampdu_factor = IEEE80211_HT_MAX_AMPDU_64K,
	.ampdu_density = IEEE80211_HT_MPDU_DENSITY_16,
	.mcs = {
		.rx_mask = { 0xff, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
		.rx_highest = cpu_to_le16(150),
		.tx_params = IEEE80211_HT_MCS_TX_DEFINED,
	},
};

/* --- ieee80211_ops callbacks --- */

static int rtl8188_op_start(struct ieee80211_hw *hw)
{
	struct rtl8188_priv *priv = hw->priv;
	u16 cr;

	/* Enable all MAC DMA/TX/RX */
	rtl8188_read16(priv, REG_CR, &cr);
	cr |= CR_HCI_TXDMA_ENABLE | CR_HCI_RXDMA_ENABLE |
	      CR_TXDMA_ENABLE | CR_RXDMA_ENABLE |
	      CR_PROTOCOL_ENABLE | CR_SCHEDULE_ENABLE |
	      CR_MAC_TX_ENABLE | CR_MAC_RX_ENABLE |
	      CR_SECURITY_ENABLE;
	rtl8188_write16(priv, REG_CR, cr);

	/* Set RX driver info size (PHY stats): 4 DW = 32 bytes */
	rtl8188_write8(priv, REG_RX_DRVINFO_SZ, 4);

	/* Accept all frames: phys match, broadcast, multicast, management,
	 * data. Do NOT filter by BSSID here - let configure_filter handle it.
	 * This ensures beacons and probe responses are received during scan. */
	priv->rx_config = RCR_ACCEPT_PHYS_MATCH | RCR_ACCEPT_MCAST |
			  RCR_ACCEPT_BCAST | RCR_ACCEPT_MGMT_FRAME |
			  RCR_ACCEPT_DATA_FRAME |
			  RCR_APPEND_PHYSTAT;
	rtl8188_write32(priv, REG_RCR, priv->rx_config);

	/*
	 * CRITICAL: Enable hardware frame subtype filters.
	 * Without these, hardware blocks ALL frames at DMA level regardless
	 * of what RCR says. Default after reset is 0x0000 (block everything).
	 *   RXFLTMAP0 = 0xffff: accept all management subtypes (beacons,
	 *                        probe responses, auth, assoc, etc.)
	 *   RXFLTMAP1 = 0x0400: accept PS-Poll (AP mode; harmless in STA)
	 *   RXFLTMAP2 = 0xffff: accept all data subtypes
	 */
	rtl8188_write16(priv, REG_RXFLTMAP0, 0xffff);
	rtl8188_write16(priv, REG_RXFLTMAP1, 0x0400);
	rtl8188_write16(priv, REG_RXFLTMAP2, 0xffff);

	/* Reset debug counters */
	priv->rx_urb_count = 0;
	priv->rx_byte_count = 0;
	priv->rx_frame_count = 0;
	priv->rx_err_count = 0;
	priv->tx_count = 0;

	/* Start receiving */
	rtl8188_rx_start(priv);

	dev_info(&priv->udev->dev, "[RTL8188] mac80211 started\n");
	return 0;
}

static void rtl8188_op_stop(struct ieee80211_hw *hw, bool suspend)
{
	struct rtl8188_priv *priv = hw->priv;

	dev_info(&priv->udev->dev,
		 "[RTL8188] Stats: TX=%u RX_URB=%u RX_BYTES=%u RX_FRAMES=%u RX_ERR=%u\n",
		 priv->tx_count, priv->rx_urb_count, priv->rx_byte_count,
		 priv->rx_frame_count, priv->rx_err_count);

	/* Block all frames at hardware level before stopping URBs */
	rtl8188_write16(priv, REG_RXFLTMAP0, 0x0000);
	rtl8188_write16(priv, REG_RXFLTMAP2, 0x0000);

	rtl8188_rx_stop(priv);

	/* Disable RX */
	rtl8188_write32(priv, REG_RCR, 0);

	/* Pause TX */
	rtl8188_write8(priv, REG_TXPAUSE, 0xff);

	dev_info(&priv->udev->dev, "[RTL8188] mac80211 stopped\n");
}

static void rtl8188_op_tx(struct ieee80211_hw *hw,
			  struct ieee80211_tx_control *control,
			  struct sk_buff *skb)
{
	struct rtl8188_priv *priv = hw->priv;
	struct ieee80211_hdr *hdr = (struct ieee80211_hdr *)skb->data;

	priv->tx_count++;
	if (ieee80211_is_probe_req(hdr->frame_control))
		dev_info(&priv->udev->dev,
			 "[RTL8188] TX probe req #%u (%d bytes)\n",
			 priv->tx_count, skb->len);

	rtl8188_tx_submit(hw, skb);
}

static int rtl8188_op_add_interface(struct ieee80211_hw *hw,
				    struct ieee80211_vif *vif)
{
	struct rtl8188_priv *priv = hw->priv;
	int i;

	/* Write MAC address to hardware */
	for (i = 0; i < ETH_ALEN; i++)
		rtl8188_write8(priv, REG_MACID + i, vif->addr[i]);

	/* Set link type to station */
	rtl8188_write8(priv, REG_MSR,
		       (MSR_LINKTYPE_NONE & 0xfc) | MSR_LINKTYPE_STATION);

	return 0;
}

static void rtl8188_op_remove_interface(struct ieee80211_hw *hw,
					struct ieee80211_vif *vif)
{
	struct rtl8188_priv *priv = hw->priv;

	rtl8188_write8(priv, REG_MSR, MSR_LINKTYPE_NONE);
}

static int rtl8188_op_config(struct ieee80211_hw *hw, int radio_idx,
			     u32 changed)
{
	struct rtl8188_priv *priv = hw->priv;

	(void)radio_idx;

	if (changed & IEEE80211_CONF_CHANGE_CHANNEL) {
		int channel = hw->conf.chandef.chan->hw_value;
		int freq = hw->conf.chandef.chan->center_freq;

		dev_info(&priv->udev->dev,
			 "[RTL8188] Channel -> %d (%d MHz)\n", channel, freq);
		rtl8188_set_channel(priv, channel);
	}

	return 0;
}

static void rtl8188_op_configure_filter(struct ieee80211_hw *hw,
					unsigned int changed_flags,
					unsigned int *total_flags,
					u64 multicast)
{
	struct rtl8188_priv *priv = hw->priv;
	u32 rcr = priv->rx_config;

	dev_info(&priv->udev->dev,
		 "[RTL8188] configure_filter: flags=0x%x (BCN_PROMISC=%d)\n",
		 *total_flags,
		 !!(*total_flags & FIF_BCN_PRBRESP_PROMISC));

	/* Adjust filter flags */
	if (*total_flags & FIF_ALLMULTI)
		rcr |= RCR_ACCEPT_MCAST;
	else
		rcr &= ~RCR_ACCEPT_MCAST;

	if (*total_flags & FIF_BCN_PRBRESP_PROMISC)
		rcr &= ~(RCR_CHECK_BSSID_MATCH | RCR_CHECK_BSSID_BEACON);
	else
		rcr |= RCR_CHECK_BSSID_MATCH | RCR_CHECK_BSSID_BEACON;

	if (*total_flags & FIF_CONTROL)
		rcr |= RCR_ACCEPT_CTRL_FRAME;

	if (*total_flags & FIF_OTHER_BSS)
		rcr |= RCR_ACCEPT_AP;
	else
		rcr &= ~RCR_ACCEPT_AP;

	if (*total_flags & FIF_FCSFAIL)
		rcr &= ~RCR_APPEND_FCS;

	priv->rx_config = rcr;
	rtl8188_write32(priv, REG_RCR, rcr);

	*total_flags &= FIF_ALLMULTI | FIF_BCN_PRBRESP_PROMISC |
			FIF_CONTROL | FIF_OTHER_BSS | FIF_FCSFAIL;
}

static void rtl8188_op_bss_info_changed(struct ieee80211_hw *hw,
					struct ieee80211_vif *vif,
					struct ieee80211_bss_conf *info,
					u64 changed)
{
	struct rtl8188_priv *priv = hw->priv;

	if (changed & BSS_CHANGED_BSSID) {
		int i;

		for (i = 0; i < ETH_ALEN; i++)
			rtl8188_write8(priv, REG_BSSID + i, info->bssid[i]);
	}

	if (changed & BSS_CHANGED_ASSOC) {
		if (vif->cfg.assoc) {
			rtl8188_write8(priv, REG_MSR,
				       MSR_LINKTYPE_STATION);
		} else {
			rtl8188_write8(priv, REG_MSR,
				       MSR_LINKTYPE_NONE);
		}
	}

	if (changed & BSS_CHANGED_BEACON_INT) {
		rtl8188_write16(priv, REG_BCN_INTERVAL,
				info->beacon_int);
	}
}

static int rtl8188_op_set_key(struct ieee80211_hw *hw,
			      enum set_key_cmd cmd,
			      struct ieee80211_vif *vif,
			      struct ieee80211_sta *sta,
			      struct ieee80211_key_conf *key)
{
	/* Let mac80211 handle encryption in software */
	return 1;
}

static int rtl8188_op_ampdu_action(struct ieee80211_hw *hw,
				   struct ieee80211_vif *vif,
				   struct ieee80211_ampdu_params *params)
{
	return -EOPNOTSUPP;
}

static void rtl8188_op_wake_tx_queue(struct ieee80211_hw *hw,
				     struct ieee80211_txq *txq)
{
	ieee80211_handle_wake_tx_queue(hw, txq);
}

/* --- ieee80211_ops struct --- */

const struct ieee80211_ops rtl8188_ops = {
	.start			= rtl8188_op_start,
	.stop			= rtl8188_op_stop,
	.tx			= rtl8188_op_tx,
	.add_interface		= rtl8188_op_add_interface,
	.remove_interface	= rtl8188_op_remove_interface,
	.config			= rtl8188_op_config,
	.configure_filter	= rtl8188_op_configure_filter,
	.bss_info_changed	= rtl8188_op_bss_info_changed,
	.set_key		= rtl8188_op_set_key,
	.ampdu_action		= rtl8188_op_ampdu_action,
	.wake_tx_queue		= rtl8188_op_wake_tx_queue,
	.add_chanctx		= ieee80211_emulate_add_chanctx,
	.remove_chanctx		= ieee80211_emulate_remove_chanctx,
	.change_chanctx		= ieee80211_emulate_change_chanctx,
	.switch_vif_chanctx	= ieee80211_emulate_switch_vif_chanctx,
};

/* --- Band / rate / capability setup --- */

int rtl8188_mac_setup(struct rtl8188_priv *priv)
{
	struct ieee80211_hw *hw = priv->hw;

	/* Copy channel and rate tables into priv so they're persistent */
	memcpy(priv->channels_2ghz, rtl8188_channels_2ghz,
	       sizeof(rtl8188_channels_2ghz));
	memcpy(priv->rates_2ghz, rtl8188_rates_2ghz,
	       sizeof(rtl8188_rates_2ghz));

	priv->band_2ghz.band = NL80211_BAND_2GHZ;
	priv->band_2ghz.channels = priv->channels_2ghz;
	priv->band_2ghz.n_channels = ARRAY_SIZE(rtl8188_channels_2ghz);
	priv->band_2ghz.bitrates = priv->rates_2ghz;
	priv->band_2ghz.n_bitrates = ARRAY_SIZE(rtl8188_rates_2ghz);
	priv->band_2ghz.ht_cap = rtl8188_ht_cap;

	hw->wiphy->bands[NL80211_BAND_2GHZ] = &priv->band_2ghz;

	/* Hardware capabilities */
	ieee80211_hw_set(hw, RX_INCLUDES_FCS);
	ieee80211_hw_set(hw, SIGNAL_DBM);
	ieee80211_hw_set(hw, MFP_CAPABLE);

	hw->wiphy->interface_modes = BIT(NL80211_IFTYPE_STATION);
	hw->wiphy->max_scan_ssids = 4;
	hw->wiphy->max_scan_ie_len = 2304;

	hw->extra_tx_headroom = RTL8188_TX_DESC_SIZE;
	hw->queues = 4;

	hw->max_rates = 1;
	hw->max_rate_tries = 11;

	SET_IEEE80211_PERM_ADDR(hw, priv->mac_addr);

	return 0;
}
