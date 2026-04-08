/* SPDX-License-Identifier: GPL-2.0 */
#ifndef RTL8188_H
#define RTL8188_H

#include <linux/types.h>
#include <linux/usb.h>
#include <linux/mutex.h>
#include <linux/workqueue.h>
#include <net/mac80211.h>

#define RTL8188_USB_VENDOR_ID		0x0bda
#define RTL8188_USB_PRODUCT_ID		0x0179
#define RTL8188_FW_NAME			"rtlwifi/rtl8188eufw.bin"
#define RTL8188_FW_HDR_SIZE		32
#define RTL8188_FW_PAGE_SIZE		4096

#define RTL8188_USB_CMD_REQ		0x05
#define RTL8188_USB_CMD_READ		0xc0
#define RTL8188_USB_CMD_WRITE		0x40
#define RTL8188_USB_CMD_TIMEOUT		500

#define RTL8188_NUM_RX_URBS		32
#define RTL8188_RX_BUF_SIZE		(32 * 1024)
#define RTL8188_MAX_REG_POLL		500

#define RTL8188_TX_DESC_SIZE		32
#define RTL8188_RX_DESC_SIZE		16

/* --- System registers (0x0000 - 0x00FF) --- */
#define REG_SYS_ISO_CTRL		0x0000
#define REG_SYS_FUNC			0x0002
#define  SYS_FUNC_BBRSTB		BIT(0)
#define  SYS_FUNC_BB_GLB_RSTN		BIT(1)
#define  SYS_FUNC_USBA			BIT(2)
#define  SYS_FUNC_USBD			BIT(4)
#define  SYS_FUNC_CPU_ENABLE		BIT(10)
#define  SYS_FUNC_DIO_RF		BIT(13)

#define REG_APS_FSMCO			0x0004
#define  APS_FSMCO_MAC_ENABLE		BIT(8)
#define  APS_FSMCO_MAC_OFF		BIT(9)
#define  APS_FSMCO_HW_SUSPEND		BIT(11)
#define  APS_FSMCO_PCIE			BIT(12)
#define  APS_FSMCO_HW_POWERDOWN	BIT(15)

#define REG_SYS_CLKR			0x0008
#define REG_AFE_MISC			0x0010
#define REG_SPS0_CTRL			0x0011
#define REG_RSV_CTRL			0x001c

#define REG_RF_CTRL			0x001f
#define  RF_ENABLE			BIT(0)
#define  RF_RSTB			BIT(1)
#define  RF_SDMRSTB			BIT(2)

#define REG_LDOA15_CTRL			0x0020
#define REG_LDOV12D_CTRL		0x0021
#define REG_LPLDO_CTRL			0x0023

#define REG_AFE_XTAL_CTRL		0x0024
#define REG_AFE_PLL_CTRL		0x0028

#define REG_9346CR			0x000a
#define  EEPROM_BOOT			BIT(4)
#define  EEPROM_ENABLE			BIT(5)

#define REG_EFUSE_CTRL			0x0030
#define REG_EFUSE_TEST			0x0034

#define REG_EFUSE_ACCESS		0x00cf
#define  EFUSE_ACCESS_ENABLE		0x69
#define  EFUSE_ACCESS_DISABLE		0x00

#define EFUSE_MAP_LEN			512
#define EFUSE_REAL_CONTENT_LEN		512
#define EFUSE_MAX_WORD_UNIT		4
#define EFUSE_MAC_ADDR_OFFSET		0xd7

#define REG_MCU_FW_DL			0x0080
#define  MCU_FW_DL_ENABLE		BIT(0)
#define  MCU_FW_DL_READY		BIT(1)
#define  MCU_FW_DL_CSUM_REPORT		BIT(2)
#define  MCU_WINT_INIT_READY		BIT(6)
#define  MCU_FW_RAM_SEL			BIT(7)
#define  MCU_CP_RESET			BIT(23)

#define REG_SYS_CFG			0x00f0

/* --- MACTOP registers (0x0100 - 0x01FF) --- */
#define REG_CR				0x0100
#define  CR_HCI_TXDMA_ENABLE		BIT(0)
#define  CR_HCI_RXDMA_ENABLE		BIT(1)
#define  CR_TXDMA_ENABLE		BIT(2)
#define  CR_RXDMA_ENABLE		BIT(3)
#define  CR_PROTOCOL_ENABLE		BIT(4)
#define  CR_SCHEDULE_ENABLE		BIT(5)
#define  CR_MAC_TX_ENABLE		BIT(6)
#define  CR_MAC_RX_ENABLE		BIT(7)
#define  CR_SW_BEACON_ENABLE		BIT(8)
#define  CR_SECURITY_ENABLE		BIT(9)
#define  CR_CALTIMER_ENABLE		BIT(10)

#define REG_MSR				0x0102
#define  MSR_LINKTYPE_NONE		0x0
#define  MSR_LINKTYPE_STATION		0x2

#define REG_PBP				0x0104
#define  PBP_PAGE_SIZE_128		0x1

#define REG_TRXDMA_CTRL			0x010c
#define REG_TRXFF_BNDY			0x0114

#define REG_HIMR			0x0120
#define REG_HISR			0x0124

#define REG_RQPN			0x0200
#define  RQPN_LOAD			BIT(31)

#define REG_TDECTRL			0x0208
#define REG_AUTO_LLT			0x0224
#define  AUTO_LLT_INIT_LLT		BIT(16)

#define REG_RXDMA_AGG_PG_TH		0x0280
#define REG_RXPKT_NUM			0x0284

#define REG_TRXDMA_CTRL_RXDMA_AGG_EN	BIT(2)

/* --- Protocol registers (0x0400 - 0x04FF) --- */
#define REG_FWHW_TXQ_CTRL		0x0420
#define REG_SPEC_SIFS			0x0428
#define REG_RETRY_LIMIT			0x042a
#define REG_RESPONSE_RATE_SET		0x0440

#define REG_EDCA_VO_PARAM		0x0500
#define REG_EDCA_VI_PARAM		0x0504
#define REG_EDCA_BE_PARAM		0x0508
#define REG_EDCA_BK_PARAM		0x050c
#define REG_SIFS_CCK			0x0514
#define REG_SIFS_OFDM			0x0516
#define REG_SLOT			0x051b
#define REG_TXPAUSE			0x0522
#define REG_BCN_INTERVAL		0x0554

/* --- WMAC registers (0x0600 - 0x07FF) --- */
#define REG_BW_OPMODE			0x0603
#define  BW_OPMODE_20MHZ		BIT(2)

#define REG_RCR				0x0608
#define  RCR_ACCEPT_AP			BIT(0)
#define  RCR_ACCEPT_PHYS_MATCH		BIT(1)
#define  RCR_ACCEPT_MCAST		BIT(2)
#define  RCR_ACCEPT_BCAST		BIT(3)
#define  RCR_CHECK_BSSID_MATCH		BIT(6)
#define  RCR_CHECK_BSSID_BEACON		BIT(7)
#define  RCR_ACCEPT_DATA_FRAME		BIT(11)
#define  RCR_ACCEPT_CTRL_FRAME		BIT(12)
#define  RCR_ACCEPT_MGMT_FRAME		BIT(13)
#define  RCR_HTC_LOC_CTRL		BIT(14)
#define  RCR_APPEND_PHYSTAT		BIT(28)
#define  RCR_APPEND_ICV			BIT(29)
#define  RCR_APPEND_FCS			BIT(31)

#define REG_RX_DRVINFO_SZ		0x060f
#define REG_MACID			0x0610
#define REG_BSSID			0x0618
#define REG_MAR				0x0620
#define REG_USTIME_EDCA			0x0638
#define REG_MAC_SPEC_SIFS		0x063a
#define REG_ACKTO			0x0640

#define REG_SECURITY_CFG		0x0680
#define REG_RXFLTMAP0			0x06a0
#define REG_RXFLTMAP1			0x06a2
#define REG_RXFLTMAP2			0x06a4

/* --- BB/PHY registers (0x0800+) --- */
#define REG_FPGA0_RF_MODE		0x0800
#define  FPGA_RF_MODE			BIT(0)

#define REG_FPGA0_TX_INFO		0x0804
#define REG_FPGA0_POWER_SAVE		0x0818

#define REG_FPGA0_XA_HSSI_PARM1	0x0820
#define REG_FPGA0_XA_HSSI_PARM2	0x0824
#define  FPGA0_HSSI_PARM2_ADDR_SHIFT	23
#define  FPGA0_HSSI_PARM2_EDGE_READ	BIT(31)

#define REG_FPGA0_XA_LSSI_PARM		0x0840
#define  FPGA0_LSSI_PARM_ADDR_SHIFT	20
#define  FPGA0_LSSI_PARM_DATA_MASK	0x000fffff

#define REG_FPGA0_XA_LSSI_READBACK	0x08a0

#define REG_FPGA1_RF_MODE		0x0900

#define REG_CCK0_SYSTEM			0x0a00
#define  CCK0_SIDEBAND			BIT(4)

#define REG_OFDM0_TRX_PATH_ENABLE	0x0c04
#define  OFDM_RF_PATH_RX_A		BIT(0)
#define  OFDM_RF_PATH_TX_A		BIT(4)

#define REG_OFDM0_XA_AGC_CORE1		0x0c50

#define REG_OFDM0_AGC_RSSI_TABLE	0x0c78

#define REG_OFDM1_LSTF			0x0d00
#define  OFDM_LSTF_PRIME_CH_LOW		BIT(10)
#define  OFDM_LSTF_PRIME_CH_HIGH	BIT(11)
#define  OFDM_LSTF_PRIME_CH_MASK	(OFDM_LSTF_PRIME_CH_LOW | \
					 OFDM_LSTF_PRIME_CH_HIGH)

/* RF register for channel/BW */
#define RF6052_REG_MODE_AG		0x18
#define  MODE_AG_CHANNEL_MASK		0x3ff
#define  MODE_AG_BW_MASK		(BIT(10) | BIT(11))
#define  MODE_AG_BW_20MHZ		(BIT(10) | BIT(11))

/* Firmware download start address */
#define REG_FW_START_ADDRESS		0x1000

/* USB-specific */
#define REG_USB_SPECIAL_OPTION		0xfe55
#define  USB_SPEC_USB_AGG_ENABLE	BIT(3)

/* 8188E early mode */
#define REG_EARLY_MODE_CONTROL_8188E	0x04d0

/* TX report */
#define REG_TX_REPORT_CTRL		0x04ec
#define  TX_REPORT_CTRL_TIMER_ENABLE	BIT(1)
#define REG_TX_REPORT_TIME		0x04f0

#define REG_32K_CTRL			0x0194

/* Dual TSF */
#define REG_DUAL_TSF_RST		0x0553
#define  DUAL_TSF_TX_OK			BIT(5)

#define REG_SCH_TX_CMD			0x05f8

/* --- TX descriptor (32 bytes) --- */
struct rtl8188_tx_desc {
	__le16 pkt_size;
	u8 pkt_offset;
	u8 txdw0;
	__le32 txdw1;
	__le32 txdw2;
	__le32 txdw3;
	__le32 txdw4;
	__le32 txdw5;
	__le32 txdw6;
	__le16 csum;
	__le16 txdw7;
} __packed;

#define TXDESC_OWN			BIT(7)
#define TXDESC_FIRST_SEGMENT		BIT(3)
#define TXDESC_LAST_SEGMENT		BIT(2)
#define TXDESC_BROADMULTICAST		BIT(0)

#define TXDESC_QUEUE_SHIFT		8
#define TXDESC_QUEUE_BE			0x0
#define TXDESC_QUEUE_MGNT		0x12
#define TXDESC32_USE_DRIVER_RATE	BIT(8)
#define TXDESC32_ACK_REPORT		BIT(19)
#define TXDESC32_HW_SEQ_ENABLE		BIT(7)
#define TXDESC32_SEQ_SHIFT		16
#define TXDESC32_SEQ_MASK		0x0fff0000
#define TXDESC32_SHORT_PREAMBLE		BIT(24)
#define TXDESC_DATA_BW			BIT(25)
#define TXDESC32_SHORT_GI		BIT(6)
#define TXDESC32_RETRY_LIMIT_ENABLE	BIT(17)

/* RX descriptor word 0 fields (RTL8188EU uses 16-byte rx descriptor) */
#define RXDESC_PKT_LEN_MASK		0x00003fff
#define RXDESC_CRC32			BIT(14)
#define RXDESC_ICV_ERR			BIT(15)
#define RXDESC_DRVINFO_SZ_SHIFT		16
#define RXDESC_DRVINFO_SZ_MASK		0x000f0000
#define RXDESC_SECURITY_SHIFT		20
#define RXDESC_QOS			BIT(23)
#define RXDESC_SHIFT_SHIFT		24
#define RXDESC_SHIFT_MASK		0x03000000
#define RXDESC_PHY_STATS		BIT(26)
#define RXDESC_SWDEC			BIT(27)
#define RXDESC_OWN			BIT(31)

/* RX descriptor word 3 */
#define RXDESC_RXMCS_MASK		0x3f
#define RXDESC_RXHT			BIT(6)
#define RXDESC_SPLCP			BIT(8)
#define RXDESC_BW			BIT(9)

/* Init table entry types */
struct rtl8188_reg8val {
	u16 reg;
	u8 val;
};

struct rtl8188_reg32val {
	u16 reg;
	u32 val;
};

struct rtl8188_rfregval {
	u8 reg;
	u32 val;
};

/* TX URB context */
struct rtl8188_tx_urb {
	struct urb urb;
	struct ieee80211_hw *hw;
	struct sk_buff *skb;
};

/* RX URB context */
struct rtl8188_rx_urb {
	struct urb urb;
	struct ieee80211_hw *hw;
	u8 *buf;
};

/* Driver private data (embedded in ieee80211_hw) */
struct rtl8188_priv {
	struct ieee80211_hw *hw;
	struct usb_device *udev;
	struct usb_interface *intf;

	struct mutex io_mutex;
	u8 *io_buf;

	unsigned int pipe_in;
	unsigned int pipe_out;     /* EP2: high priority (mgmt/VI/VO) */
	unsigned int pipe_out_be;  /* EP3: normal priority (BE/BK) */

	u8 mac_addr[ETH_ALEN];

	const struct firmware *fw;

	u8 efuse_map[EFUSE_MAP_LEN];

	struct rtl8188_rx_urb rx_urbs[RTL8188_NUM_RX_URBS];
	bool shutdown;

	struct ieee80211_supported_band band_2ghz;
	struct ieee80211_channel channels_2ghz[14];
	struct ieee80211_rate rates_2ghz[12];
	struct ieee80211_sta_ht_cap ht_cap_2ghz;

	u32 rx_config;

	/* Debug counters */
	u32 rx_urb_count;
	u32 rx_byte_count;
	u32 rx_frame_count;
	u32 rx_err_count;
	u32 tx_count;
};

/* --- Function prototypes --- */

/* rtl8188_main.c */
int rtl8188_read8(struct rtl8188_priv *priv, u16 addr, u8 *val);
int rtl8188_read16(struct rtl8188_priv *priv, u16 addr, u16 *val);
int rtl8188_read32(struct rtl8188_priv *priv, u16 addr, u32 *val);
int rtl8188_write8(struct rtl8188_priv *priv, u16 addr, u8 val);
int rtl8188_write16(struct rtl8188_priv *priv, u16 addr, u16 val);
int rtl8188_write32(struct rtl8188_priv *priv, u16 addr, u32 val);
int rtl8188_write_block(struct rtl8188_priv *priv, u16 addr,
			const u8 *buf, u16 len);

/* rtl8188_hw.c */
int rtl8188_hw_init(struct rtl8188_priv *priv);
int rtl8188_read_mac_addr(struct rtl8188_priv *priv);

/* rtl8188_phy.c */
int rtl8188_phy_init(struct rtl8188_priv *priv);
int rtl8188_set_channel(struct rtl8188_priv *priv, int channel);
void rtl8188_rf_write(struct rtl8188_priv *priv, u8 reg, u32 data);
u32 rtl8188_rf_read(struct rtl8188_priv *priv, u8 reg);

/* rtl8188_mac.c */
int rtl8188_mac_setup(struct rtl8188_priv *priv);
extern const struct ieee80211_ops rtl8188_ops;

/* rtl8188_trx.c */
int rtl8188_rx_start(struct rtl8188_priv *priv);
void rtl8188_rx_stop(struct rtl8188_priv *priv);
void rtl8188_tx_submit(struct ieee80211_hw *hw, struct sk_buff *skb);

#endif /* RTL8188_H */
