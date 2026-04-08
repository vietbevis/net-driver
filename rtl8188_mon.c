// SPDX-License-Identifier: GPL-2.0
/*
 * RTL8188ETV WiFi Companion Monitor Module
 *
 * Runs alongside rtl8xxxu to provide:
 *  - USB device detection and hardware info
 *  - WiFi scan / connect / disconnect via char device
 *  - Real-time packet monitoring and statistics
 *  - /proc filesystem entries for device status
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/usb.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/if_ether.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/workqueue.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/uaccess.h>
#include <linux/kmod.h>
#include <linux/wait.h>
#include <linux/jiffies.h>
#include <linux/string.h>
#include <linux/rtnetlink.h>
#include <linux/ip.h>
#include <linux/ipv6.h>
#include <linux/tcp.h>
#include <linux/udp.h>
#include <linux/inet.h>

#define DEVICE_NAME       "rtl8188"
#define TARGET_VENDOR     0x0bda
#define TARGET_PRODUCT    0x0179
#define RESP_BUF_SIZE     (32 * 1024)
#define SCAN_FILE         "/tmp/.rtl8188_scan"
#define STATUS_FILE       "/tmp/.rtl8188_status"
#define CONNECT_FILE      "/tmp/.rtl8188_connect"
#define CAPTURE_RING_SIZE 128
#define MAX_CMD_LEN       256
#define PAYLOAD_SNAP_SIZE 1280   /* capture full chat_frame header (52B) + 76B payload */
#define CHAT_PORT         9090

/* chat_frame wire format constants (mirrors crypto_chat.h) */
#define CF_HDR_SIZE   52   /* 1+1+2+16+32 */
#define CF_OFF_VER     0
#define CF_OFF_TYPE    1
#define CF_OFF_PLEN    2   /* __u16, host byte order */
#define CF_OFF_IV      4   /* 16 bytes */
#define CF_OFF_HMAC   20   /* 32 bytes */
#define CF_OFF_PAYLOAD 52  /* encrypted data */

#define MSG_TYPE_AUTH      0x01
#define MSG_TYPE_AUTH_OK   0x02
#define MSG_TYPE_AUTH_FAIL 0x03
#define MSG_TYPE_CHAT      0x04
#define MSG_TYPE_SYSTEM    0x05
#define MSG_TYPE_LOGOUT    0x06
#define MSG_TYPE_LIST      0x07
#define MSG_TYPE_BROADCAST 0x08
#define MSG_TYPE_REGISTER  0x09
#define MSG_TYPE_REG_OK    0x0A
#define MSG_TYPE_REG_FAIL  0x0B

static char *helper_envp[] = {
	"HOME=/",
	"TERM=linux",
	"PATH=/sbin:/bin:/usr/sbin:/usr/bin",
	NULL
};

/* --- Data structures --- */

struct pkt_entry {
	unsigned long tstamp;
	u8 src_mac[ETH_ALEN];
	u8 dst_mac[ETH_ALEN];
	__be16 eth_proto;
	unsigned int len;
	/* IP-level info */
	__be32 src_ip;
	__be32 dst_ip;
	u8  ip_proto;		/* IPPROTO_TCP, UDP, ICMP ... */
	__be16 src_port;
	__be16 dst_port;
	/* Payload snapshot for chat packet deep inspection */
	u8  payload[PAYLOAD_SNAP_SIZE];
	int payload_len;
	int is_chat;		/* 1 if src/dst port == CHAT_PORT */

	/* Parsed chat_frame fields (valid when is_chat && chat_parsed) */
	int chat_parsed;
	u8  chat_ver;
	u8  chat_type;
	u16 chat_plen;          /* payload_len field from frame */
	u8  chat_iv[16];        /* AES IV */
	u8  chat_hmac[32];      /* HMAC-SHA256 (full 32 bytes) */
	u8  chat_enc[32];       /* first 32 bytes of encrypted payload */
	int chat_enc_len;       /* bytes available in chat_enc (max 32) */
};

struct rtl8188_mon {
	/* USB device */
	struct usb_device *udev;
	bool dev_present;
	struct notifier_block usb_nb;

	/* Network interface */
	struct net_device *ndev;
	char ifname[IFNAMSIZ];
	struct notifier_block net_nb;

	/* Packet monitor */
	struct packet_type ptype;
	bool pkt_registered;
	atomic64_t rx_pkts;
	atomic64_t rx_bytes;
	atomic_t arp_cnt;
	atomic_t ip_cnt;
	atomic_t ipv6_cnt;
	atomic_t other_cnt;

	/* Capture ring buffer */
	struct pkt_entry ring[CAPTURE_RING_SIZE];
	int ring_head;
	int ring_count;
	spinlock_t ring_lock;
	bool capture_on;
	atomic_t tcp_cnt;
	atomic_t udp_cnt;
	atomic_t icmp_cnt;
	atomic_t chat_cnt;    /* always counts port 9090 */
	atomic_t filter_cnt;  /* counts packets matching filter_port */
	__be16 filter_port;   /* 0 = capture all, else only this port */

	/* Command / response */
	struct mutex cmd_lock;
	char *resp_buf;
	int resp_len;
	bool resp_ready;
	wait_queue_head_t resp_wq;

	/* Async work */
	struct workqueue_struct *wq;
	struct work_struct scan_work;
	struct work_struct connect_work;
	struct work_struct disconnect_work;
	char cmd_ssid[64];
	char cmd_pass[128];

	/* Char device */
	dev_t devno;
	struct cdev cdev;
	struct class *cls;
	struct device *chrdev;

	/* Proc */
	struct proc_dir_entry *proc_dir;

	unsigned long load_jiffies;
};

static struct rtl8188_mon *g_mon;

/* Forward declarations */
static void scan_existing_netdev(void);

/* ================================================================
 * Helper functions
 * ================================================================ */

static int run_cmd(const char *cmd)
{
	char *argv[] = { "/bin/sh", "-c", (char *)cmd, NULL };

	return call_usermodehelper(argv[0], argv, helper_envp, UMH_WAIT_PROC);
}

static int read_tmpfile(const char *path, char *buf, int size)
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

static void sanitize(char *s, int len)
{
	int i;

	for (i = 0; i < len && s[i]; i++) {
		char c = s[i];

		if (c == '\'' || c == '"' || c == '`' || c == '$' ||
		    c == ';' || c == '|' || c == '&' || c == '\\' ||
		    c == '(' || c == ')' || c == '<' || c == '>')
			s[i] = '_';
	}
}

static void set_resp(struct rtl8188_mon *mon, const char *text)
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
 * USB device detection (passive — does NOT claim the device)
 * ================================================================ */

static int find_our_usb(struct usb_device *udev, void *data)
{
	if (le16_to_cpu(udev->descriptor.idVendor) == TARGET_VENDOR &&
	    le16_to_cpu(udev->descriptor.idProduct) == TARGET_PRODUCT) {
		g_mon->udev = usb_get_dev(udev);
		g_mon->dev_present = true;
		return 1;
	}
	return 0;
}

static void scan_existing_usb(void)
{
	usb_for_each_dev(NULL, find_our_usb);
}

static int rtl8188_usb_notify(struct notifier_block *nb,
			      unsigned long action, void *data)
{
	struct usb_device *udev = data;
	struct rtl8188_mon *mon = g_mon;

	if (le16_to_cpu(udev->descriptor.idVendor) != TARGET_VENDOR ||
	    le16_to_cpu(udev->descriptor.idProduct) != TARGET_PRODUCT)
		return NOTIFY_DONE;

	switch (action) {
	case USB_DEVICE_ADD:
		if (!mon->dev_present) {
			mon->udev = usb_get_dev(udev);
			mon->dev_present = true;
			pr_info("[rtl8188_mon] USB device connected\n");
			scan_existing_netdev();
		}
		break;
	case USB_DEVICE_REMOVE:
		if (mon->udev == udev) {
			if (mon->pkt_registered) {
				dev_remove_pack(&mon->ptype);
				mon->pkt_registered = false;
			}
			mon->ndev = NULL;
			mon->ifname[0] = '\0';
			usb_put_dev(mon->udev);
			mon->udev = NULL;
			mon->dev_present = false;
			pr_info("[rtl8188_mon] USB device disconnected\n");
		}
		break;
	}
	return NOTIFY_OK;
}

/* ================================================================
 * Network interface tracking
 * ================================================================ */

static bool is_our_iface(struct net_device *ndev)
{
	struct device *d;

	if (!g_mon || !g_mon->udev)
		return false;

	for (d = ndev->dev.parent; d; d = d->parent) {
		if (d == &g_mon->udev->dev)
			return true;
	}
	return false;
}

static int rtl8188_net_notify(struct notifier_block *nb,
			      unsigned long event, void *ptr)
{
	struct net_device *dev = netdev_notifier_info_to_dev(ptr);
	struct rtl8188_mon *mon = g_mon;

	if (!mon || !is_our_iface(dev))
		return NOTIFY_DONE;

	switch (event) {
	case NETDEV_REGISTER:
		mon->ndev = dev;
		strscpy(mon->ifname, dev->name, IFNAMSIZ);
		pr_info("[rtl8188_mon] Interface %s registered\n", dev->name);
		break;
	case NETDEV_UP:
		mon->ndev = dev;
		strscpy(mon->ifname, dev->name, IFNAMSIZ);
		pr_info("[rtl8188_mon] Interface %s is UP\n", dev->name);
		break;
	case NETDEV_DOWN:
		pr_info("[rtl8188_mon] Interface %s is DOWN\n", dev->name);
		break;
	case NETDEV_UNREGISTER:
		if (mon->pkt_registered) {
			dev_remove_pack(&mon->ptype);
			mon->pkt_registered = false;
		}
		mon->ndev = NULL;
		mon->ifname[0] = '\0';
		pr_info("[rtl8188_mon] Interface unregistered\n");
		break;
	case NETDEV_CHANGENAME:
		strscpy(mon->ifname, dev->name, IFNAMSIZ);
		break;
	}
	return NOTIFY_OK;
}

static void scan_existing_netdev(void)
{
	struct net_device *dev;

	rtnl_lock();
	for_each_netdev(&init_net, dev) {
		if (is_our_iface(dev)) {
			g_mon->ndev = dev;
			strscpy(g_mon->ifname, dev->name, IFNAMSIZ);
			pr_info("[rtl8188_mon] Found existing interface %s\n",
				dev->name);
			break;
		}
	}
	rtnl_unlock();
}

/* ================================================================
 * Packet monitoring via dev_add_pack(ETH_P_ALL)
 * ================================================================ */

static int rtl8188_pkt_recv(struct sk_buff *skb, struct net_device *dev,
			    struct packet_type *pt,
			    struct net_device *orig_dev)
{
	struct rtl8188_mon *mon = g_mon;
	unsigned long flags;

	if (!mon || dev != mon->ndev)
		goto out;

	atomic64_inc(&mon->rx_pkts);
	atomic64_add(skb->len, &mon->rx_bytes);

	switch (ntohs(skb->protocol)) {
	case ETH_P_ARP:  atomic_inc(&mon->arp_cnt);   break;
	case ETH_P_IP:   atomic_inc(&mon->ip_cnt);    break;
	case ETH_P_IPV6: atomic_inc(&mon->ipv6_cnt);  break;
	default:         atomic_inc(&mon->other_cnt); break;
	}

	if (mon->capture_on) {
		struct pkt_entry *e;
		struct iphdr *iph = NULL;
		struct tcphdr *th;
		struct udphdr *uh;
		__be16 sport = 0, dport = 0;
		u8 l4_proto = 0;
		const u8 *l4_payload = NULL;
		int l4_payload_len = 0;
		int is_chat = 0;

		if (ntohs(skb->protocol) == ETH_P_IP &&
		    skb->len >= sizeof(struct iphdr)) {
			iph = ip_hdr(skb);
			l4_proto = iph->protocol;

			if (l4_proto == IPPROTO_TCP) {
				atomic_inc(&mon->tcp_cnt);
				if (skb->len >= (iph->ihl * 4 + sizeof(struct tcphdr))) {
					th = (struct tcphdr *)((u8 *)iph + iph->ihl * 4);
					sport = th->source;
					dport = th->dest;
					l4_payload = (u8 *)th + th->doff * 4;
					l4_payload_len = skb->len - (iph->ihl * 4) - (th->doff * 4);
					if (l4_payload_len < 0)
						l4_payload_len = 0;
				}
			} else if (l4_proto == IPPROTO_UDP) {
				atomic_inc(&mon->udp_cnt);
				if (skb->len >= (iph->ihl * 4 + sizeof(struct udphdr))) {
					uh = (struct udphdr *)((u8 *)iph + iph->ihl * 4);
					sport = uh->source;
					dport = uh->dest;
					l4_payload = (u8 *)uh + sizeof(struct udphdr);
					l4_payload_len = ntohs(uh->len) - sizeof(struct udphdr);
					if (l4_payload_len < 0)
						l4_payload_len = 0;
				}
			} else if (l4_proto == IPPROTO_ICMP) {
				atomic_inc(&mon->icmp_cnt);
			}

			if (ntohs(sport) == CHAT_PORT || ntohs(dport) == CHAT_PORT) {
				is_chat = 1;
				atomic_inc(&mon->chat_cnt);
			}
		}

		/* Count packets matching the active filter port */
		if (mon->filter_port != 0 &&
		    (sport == mon->filter_port || dport == mon->filter_port))
			atomic_inc(&mon->filter_cnt);

		/* Apply port filter to ring buffer: 0 = no filter */
		if (mon->filter_port != 0) {
			if (sport != mon->filter_port && dport != mon->filter_port)
				goto out;
		}

		spin_lock_irqsave(&mon->ring_lock, flags);
		e = &mon->ring[mon->ring_head];
		memset(e, 0, sizeof(*e));
		e->tstamp = jiffies;
		if (skb_mac_header_was_set(skb)) {
			struct ethhdr *eth = eth_hdr(skb);
			memcpy(e->src_mac, eth->h_source, ETH_ALEN);
			memcpy(e->dst_mac, eth->h_dest,   ETH_ALEN);
		}
		e->eth_proto = skb->protocol;
		e->len = skb->len;
		e->ip_proto = l4_proto;
		e->src_port = sport;
		e->dst_port = dport;
		e->is_chat = is_chat;

		if (iph) {
			e->src_ip = iph->saddr;
			e->dst_ip = iph->daddr;
		}

		if (l4_payload && l4_payload_len > 0) {
			int snap = l4_payload_len < PAYLOAD_SNAP_SIZE
				   ? l4_payload_len : PAYLOAD_SNAP_SIZE;
			/* Offset from start of skb->data (= network header start) */
			int off = (int)(l4_payload - (u8 *)ip_hdr(skb))
				  + skb_network_offset(skb);
			if (off >= 0 && off + snap <= (int)skb->len &&
			    skb_copy_bits(skb, off, e->payload, snap) == 0)
				e->payload_len = snap;
		}

		/* Auto-detect chat_frame by content (any port):
		 * version must be 1, type must be a known MSG_TYPE_*.
		 * This handles apps using non-default ports (e.g. 8888).
		 */
		if (!e->chat_parsed && e->payload_len >= CF_HDR_SIZE) {
			const u8 *f = e->payload;
			u8 ver  = f[CF_OFF_VER];
			u8 type = f[CF_OFF_TYPE];

			if (ver == 1 && type >= MSG_TYPE_AUTH &&
			    type <= MSG_TYPE_REG_FAIL) {
				e->chat_ver  = ver;
				e->chat_type = type;
				e->chat_plen = f[CF_OFF_PLEN] |
					       ((u16)f[CF_OFF_PLEN + 1] << 8);
				memcpy(e->chat_iv,   f + CF_OFF_IV,   16);
				memcpy(e->chat_hmac, f + CF_OFF_HMAC, 32);

				e->chat_enc_len = e->payload_len - CF_HDR_SIZE;
				if (e->chat_enc_len > 32)
					e->chat_enc_len = 32;
				if (e->chat_enc_len > 0)
					memcpy(e->chat_enc,
					       f + CF_OFF_PAYLOAD,
					       e->chat_enc_len);

				e->chat_parsed = 1;
				if (!e->is_chat) {
					e->is_chat = 1;
					atomic_inc(&mon->chat_cnt);
				}
			}
		}

		mon->ring_head = (mon->ring_head + 1) % CAPTURE_RING_SIZE;
		if (mon->ring_count < CAPTURE_RING_SIZE)
			mon->ring_count++;
		spin_unlock_irqrestore(&mon->ring_lock, flags);
	}

out:
	kfree_skb(skb);
	return 0;
}

/* ================================================================
 * Async work: scan, connect, disconnect
 * ================================================================ */

static void scan_work_fn(struct work_struct *work)
{
	struct rtl8188_mon *mon = g_mon;
	char cmd[512];
	int n, ret;

	if (!mon->ifname[0]) {
		set_resp(mon, "ERROR: No wireless interface found.\n"
			      "Make sure rtl8xxxu driver is loaded and "
			      "device is plugged in.\n");
		return;
	}

	/* Try scan trigger first; if device is busy, use cached results */
	snprintf(cmd, sizeof(cmd),
		 "/usr/sbin/ip link set %s up 2>/dev/null; "
		 "/usr/sbin/iw dev %s scan > " SCAN_FILE " 2>&1 || "
		 "/usr/sbin/iw dev %s scan dump > " SCAN_FILE " 2>&1",
		 mon->ifname, mon->ifname, mon->ifname);
	ret = run_cmd(cmd);

	mutex_lock(&mon->cmd_lock);
	n = read_tmpfile(SCAN_FILE, mon->resp_buf, RESP_BUF_SIZE);
	if (n <= 0)
		n = snprintf(mon->resp_buf, RESP_BUF_SIZE,
			     "Scan failed (ret=%d). Check iw is installed.\n",
			     ret);
	mon->resp_len = n;
	mon->resp_ready = true;
	mutex_unlock(&mon->cmd_lock);
	wake_up_interruptible(&mon->resp_wq);
}

static void connect_work_fn(struct work_struct *work)
{
	struct rtl8188_mon *mon = g_mon;
	char cmd[1024];
	int n;

	if (!mon->ifname[0]) {
		set_resp(mon, "ERROR: No wireless interface found.\n");
		return;
	}

	if (mon->cmd_pass[0]) {
		/* WPA/WPA2: create config, start wpa_supplicant */
		snprintf(cmd, sizeof(cmd),
			 "killall wpa_supplicant 2>/dev/null\n"
			 "cat > /tmp/.rtl8188_wpa.conf << 'WPA'\n"
			 "ctrl_interface=/var/run/wpa_supplicant\n"
			 "network={\n"
			 "    ssid=\"%s\"\n"
			 "    psk=\"%s\"\n"
			 "}\n"
			 "WPA\n"
			 "wpa_supplicant -B -i %s -c /tmp/.rtl8188_wpa.conf "
			 "2>&1 > " CONNECT_FILE "\n"
			 "sleep 3\n"
			 "dhclient %s 2>/dev/null || true\n"
			 "/usr/sbin/iw dev %s link >> " CONNECT_FILE " 2>&1",
			 mon->cmd_ssid, mon->cmd_pass,
			 mon->ifname, mon->ifname, mon->ifname);
	} else {
		/* Open network */
		snprintf(cmd, sizeof(cmd),
			 "/usr/sbin/iw dev %s connect '%s' "
			 "2>&1 > " CONNECT_FILE "\n"
			 "sleep 1\n"
			 "/usr/sbin/iw dev %s link >> " CONNECT_FILE " 2>&1",
			 mon->ifname, mon->cmd_ssid, mon->ifname);
	}

	run_cmd(cmd);

	mutex_lock(&mon->cmd_lock);
	n = 0;
	n += snprintf(mon->resp_buf + n, RESP_BUF_SIZE - n,
		      "Connecting to '%s'%s...\n\n",
		      mon->cmd_ssid,
		      mon->cmd_pass[0] ? " (WPA)" : " (Open)");
	n += read_tmpfile(CONNECT_FILE, mon->resp_buf + n, RESP_BUF_SIZE - n);
	mon->resp_len = n;
	mon->resp_ready = true;
	mutex_unlock(&mon->cmd_lock);
	wake_up_interruptible(&mon->resp_wq);
}

static void disconnect_work_fn(struct work_struct *work)
{
	struct rtl8188_mon *mon = g_mon;
	char cmd[256];

	if (!mon->ifname[0]) {
		set_resp(mon, "ERROR: No wireless interface found.\n");
		return;
	}

	snprintf(cmd, sizeof(cmd),
		 "killall wpa_supplicant 2>/dev/null; "
		 "/usr/sbin/iw dev %s disconnect 2>&1",
		 mon->ifname);
	run_cmd(cmd);
	set_resp(mon, "Disconnected.\n");
}

/* ================================================================
 * Synchronous response generators
 * ================================================================ */

static void generate_info(struct rtl8188_mon *mon)
{
	int n = 0;

	mutex_lock(&mon->cmd_lock);

	n += snprintf(mon->resp_buf + n, RESP_BUF_SIZE - n,
		      "============================================\n"
		      "   RTL8188ETV USB WiFi Device Information\n"
		      "============================================\n\n");

	if (mon->dev_present && mon->udev) {
		struct usb_device *u = mon->udev;

		n += snprintf(mon->resp_buf + n, RESP_BUF_SIZE - n,
			      "  Status:       CONNECTED\n"
			      "  Vendor:       0x%04x (%s)\n"
			      "  Product:      0x%04x (%s)\n"
			      "  USB Speed:    %s\n",
			      le16_to_cpu(u->descriptor.idVendor),
			      u->manufacturer ? u->manufacturer : "Realtek",
			      le16_to_cpu(u->descriptor.idProduct),
			      u->product ? u->product : "RTL8188ETV",
			      usb_speed_string(u->speed));

		if (u->actconfig) {
			struct usb_host_interface *iface =
				u->actconfig->interface[0]->cur_altsetting;
			int i;

			n += snprintf(mon->resp_buf + n, RESP_BUF_SIZE - n,
				      "  Endpoints:    %d\n",
				      iface->desc.bNumEndpoints);
			for (i = 0; i < iface->desc.bNumEndpoints; i++) {
				struct usb_endpoint_descriptor *ep =
					&iface->endpoint[i].desc;
				n += snprintf(mon->resp_buf + n,
					      RESP_BUF_SIZE - n,
					      "    EP%d: %s %s (max %d)\n",
					      ep->bEndpointAddress & 0x0f,
					      usb_endpoint_dir_in(ep) ?
						"IN " : "OUT",
					      usb_ep_type_string(
						usb_endpoint_type(ep)),
					      le16_to_cpu(ep->wMaxPacketSize));
			}
		}

		n += snprintf(mon->resp_buf + n, RESP_BUF_SIZE - n,
			      "  Interface:    %s\n",
			      mon->ifname[0] ? mon->ifname : "(none)");

		if (mon->ndev)
			n += snprintf(mon->resp_buf + n, RESP_BUF_SIZE - n,
				      "  MAC Address:  %pM\n",
				      mon->ndev->dev_addr);
	} else {
		n += snprintf(mon->resp_buf + n, RESP_BUF_SIZE - n,
			      "  Status:       NOT FOUND\n"
			      "  Plug in RTL8188ETV USB WiFi adapter.\n");
	}

	n += snprintf(mon->resp_buf + n, RESP_BUF_SIZE - n,
		      "\n  Module uptime: %lu seconds\n",
		      (jiffies - mon->load_jiffies) / HZ);

	mon->resp_len = n;
	mon->resp_ready = true;
	mutex_unlock(&mon->cmd_lock);
	wake_up_interruptible(&mon->resp_wq);
}

static void generate_status(struct rtl8188_mon *mon)
{
	char cmd[256];
	int n;

	if (!mon->ifname[0]) {
		set_resp(mon, "No wireless interface found.\n");
		return;
	}

	snprintf(cmd, sizeof(cmd),
		 "{ echo '=== Connection Status ==='; echo; "
		 "/usr/sbin/iw dev %s link 2>&1; echo; "
		 "echo '=== IP Address ==='; "
		 "/usr/sbin/ip addr show %s 2>/dev/null | grep -E 'inet ' || "
		 "echo '  (no IP address)'; } > " STATUS_FILE " 2>&1",
		 mon->ifname, mon->ifname);
	run_cmd(cmd);

	mutex_lock(&mon->cmd_lock);
	n = read_tmpfile(STATUS_FILE, mon->resp_buf, RESP_BUF_SIZE);
	if (n <= 0)
		n = snprintf(mon->resp_buf, RESP_BUF_SIZE,
			     "Unable to get status\n");
	mon->resp_len = n;
	mon->resp_ready = true;
	mutex_unlock(&mon->cmd_lock);
	wake_up_interruptible(&mon->resp_wq);
}

static void generate_stats(struct rtl8188_mon *mon)
{
	struct rtnl_link_stats64 ns;
	int n = 0;
	unsigned long uptime = (jiffies - mon->load_jiffies) / HZ;

	mutex_lock(&mon->cmd_lock);

	n += snprintf(mon->resp_buf + n, RESP_BUF_SIZE - n,
		      "============================================\n"
		      "          Packet Statistics\n"
		      "============================================\n\n");

	n += snprintf(mon->resp_buf + n, RESP_BUF_SIZE - n,
		      "  Monitor:    %s\n"
		      "  Capture:    %s\n\n",
		      mon->pkt_registered ? "ON" : "OFF",
		      mon->capture_on ? "ON" : "OFF");

	if (mon->pkt_registered) {
		n += snprintf(mon->resp_buf + n, RESP_BUF_SIZE - n,
			      "  [Monitored Traffic]\n"
			      "    RX Packets:  %lld\n"
			      "    RX Bytes:    %lld\n"
			      "    ARP:         %d\n"
			      "    IPv4:        %d\n"
			      "    IPv6:        %d\n"
			      "    Other:       %d\n\n"
			      "  [L4 Protocol]\n"
			      "    TCP:         %d\n"
			      "    UDP:         %d\n"
			      "    ICMP:        %d\n"
			      "    Chat(9090):  %d\n\n",
			      atomic64_read(&mon->rx_pkts),
			      atomic64_read(&mon->rx_bytes),
			      atomic_read(&mon->arp_cnt),
			      atomic_read(&mon->ip_cnt),
			      atomic_read(&mon->ipv6_cnt),
			      atomic_read(&mon->other_cnt),
			      atomic_read(&mon->tcp_cnt),
			      atomic_read(&mon->udp_cnt),
			      atomic_read(&mon->icmp_cnt),
			      atomic_read(&mon->chat_cnt));

		if (mon->filter_port) {
			n += snprintf(mon->resp_buf + n, RESP_BUF_SIZE - n,
				      "  [Filter: port %d]\n"
				      "    Matched pkts: %d\n\n",
				      ntohs(mon->filter_port),
				      atomic_read(&mon->filter_cnt));
		} else {
			n += snprintf(mon->resp_buf + n, RESP_BUF_SIZE - n,
				      "  Filter port:  none (all)\n\n");
		}
	}

	if (mon->ndev) {
		dev_get_stats(mon->ndev, &ns);
		n += snprintf(mon->resp_buf + n, RESP_BUF_SIZE - n,
			      "  [Driver Statistics]\n"
			      "    TX: %llu packets, %llu bytes\n"
			      "    RX: %llu packets, %llu bytes\n"
			      "    TX errors: %llu  RX errors: %llu\n"
			      "    TX dropped: %llu  RX dropped: %llu\n\n",
			      ns.tx_packets, ns.tx_bytes,
			      ns.rx_packets, ns.rx_bytes,
			      ns.tx_errors, ns.rx_errors,
			      ns.tx_dropped, ns.rx_dropped);
	}

	n += snprintf(mon->resp_buf + n, RESP_BUF_SIZE - n,
		      "  Capture buffer: %d / %d entries\n"
		      "  Module uptime:  %lu seconds\n",
		      mon->ring_count, CAPTURE_RING_SIZE, uptime);

	mon->resp_len = n;
	mon->resp_ready = true;
	mutex_unlock(&mon->cmd_lock);
	wake_up_interruptible(&mon->resp_wq);
}

static const char *chat_type_name(u8 type)
{
	switch (type) {
	case MSG_TYPE_AUTH:      return "AUTH";
	case MSG_TYPE_AUTH_OK:   return "AUTH_OK";
	case MSG_TYPE_AUTH_FAIL: return "AUTH_FAIL";
	case MSG_TYPE_CHAT:      return "CHAT";
	case MSG_TYPE_SYSTEM:    return "SYSTEM";
	case MSG_TYPE_LOGOUT:    return "LOGOUT";
	case MSG_TYPE_LIST:      return "LIST";
	case MSG_TYPE_BROADCAST: return "BROADCAST";
	case MSG_TYPE_REGISTER:  return "REGISTER";
	case MSG_TYPE_REG_OK:    return "REG_OK";
	case MSG_TYPE_REG_FAIL:  return "REG_FAIL";
	default:                 return "UNKNOWN";
	}
}

static const char *l4_proto_str(u8 proto)
{
	switch (proto) {
	case IPPROTO_TCP:  return "TCP";
	case IPPROTO_UDP:  return "UDP";
	case IPPROTO_ICMP: return "ICMP";
	default:           return "---";
	}
}

/* Print up to 16 hex bytes, grouped as 8+8 */
static int hex_dump_line(char *buf, int bufsize, const u8 *data, int len)
{
	int i, n = 0;
	int show = len > 16 ? 16 : len;

	for (i = 0; i < show && n < bufsize - 4; i++) {
		if (i == 8)
			n += snprintf(buf + n, bufsize - n, " ");
		n += snprintf(buf + n, bufsize - n, "%02x ", data[i]);
	}
	if (len > 16)
		n += snprintf(buf + n, bufsize - n, "...");
	return n;
}

static void generate_capture(struct rtl8188_mon *mon)
{
	int i, start, count, n = 0;
	unsigned long flags;
	struct pkt_entry *e;
	const char *eth_str;

	mutex_lock(&mon->cmd_lock);

	spin_lock_irqsave(&mon->ring_lock, flags);
	count = mon->ring_count;
	start = (mon->ring_head - count + CAPTURE_RING_SIZE) %
		CAPTURE_RING_SIZE;

	for (i = 0; i < count && n < RESP_BUF_SIZE - 400; i++) {
		int idx = (start + i) % CAPTURE_RING_SIZE;
		unsigned long age;

		e = &mon->ring[idx];
		age = (jiffies - e->tstamp) / HZ;

		switch (ntohs(e->eth_proto)) {
		case ETH_P_IP:   eth_str = "IPv4"; break;
		case ETH_P_IPV6: eth_str = "IPv6"; break;
		case ETH_P_ARP:  eth_str = "ARP";  break;
		case ETH_P_PAE:  eth_str = "EAPOL"; break;
		default:         eth_str = "????"; break;
		}

		/* Packet header line — TUI detects packets by leading '[' */
		n += snprintf(mon->resp_buf + n, RESP_BUF_SIZE - n,
			      "[%d] %lus | %s/%s | %uB",
			      i, age, eth_str, l4_proto_str(e->ip_proto),
			      e->len);
		if (e->is_chat)
			n += snprintf(mon->resp_buf + n, RESP_BUF_SIZE - n,
				      " [CHAT-AES]");
		n += snprintf(mon->resp_buf + n, RESP_BUF_SIZE - n, "\n");

		/* Address line */
		if (e->src_ip || e->dst_ip) {
			n += snprintf(mon->resp_buf + n, RESP_BUF_SIZE - n,
				      "ADDR:%pI4:%d->%pI4:%d\n",
				      &e->src_ip, ntohs(e->src_port),
				      &e->dst_ip, ntohs(e->dst_port));
		} else {
			n += snprintf(mon->resp_buf + n, RESP_BUF_SIZE - n,
				      "ADDR:%pM->%pM\n",
				      e->src_mac, e->dst_mac);
		}

		/* Chat frame detail */
		if (e->chat_parsed) {
			n += snprintf(mon->resp_buf + n, RESP_BUF_SIZE - n,
				      "CF_VER:%u\n"
				      "CF_TYPE:0x%02x(%s)\n"
				      "CF_PLEN:%u\n",
				      e->chat_ver,
				      e->chat_type,
				      chat_type_name(e->chat_type),
				      e->chat_plen);

			/* IV – always 16 bytes */
			n += snprintf(mon->resp_buf + n, RESP_BUF_SIZE - n,
				      "IV:");
			n += hex_dump_line(mon->resp_buf + n,
					   RESP_BUF_SIZE - n,
					   e->chat_iv, 16);
			n += snprintf(mon->resp_buf + n, RESP_BUF_SIZE - n,
				      "\n");

			/* HMAC – show all 32 bytes as two 16-byte rows */
			n += snprintf(mon->resp_buf + n, RESP_BUF_SIZE - n,
				      "HMAC1:");
			n += hex_dump_line(mon->resp_buf + n,
					   RESP_BUF_SIZE - n,
					   e->chat_hmac, 16);
			n += snprintf(mon->resp_buf + n, RESP_BUF_SIZE - n,
				      "\n");
			n += snprintf(mon->resp_buf + n, RESP_BUF_SIZE - n,
				      "HMAC2:");
			n += hex_dump_line(mon->resp_buf + n,
					   RESP_BUF_SIZE - n,
					   e->chat_hmac + 16, 16);
			n += snprintf(mon->resp_buf + n, RESP_BUF_SIZE - n,
				      "\n");

			/* ENC – up to 32 bytes as one or two 16-byte rows */
			if (e->chat_enc_len > 0) {
				int enc1 = e->chat_enc_len > 16
					   ? 16 : e->chat_enc_len;
				int enc2 = e->chat_enc_len - enc1;

				n += snprintf(mon->resp_buf + n,
					      RESP_BUF_SIZE - n, "ENC1:");
				n += hex_dump_line(mon->resp_buf + n,
						   RESP_BUF_SIZE - n,
						   e->chat_enc, enc1);
				n += snprintf(mon->resp_buf + n,
					      RESP_BUF_SIZE - n, "\n");

				if (enc2 > 0) {
					n += snprintf(mon->resp_buf + n,
						      RESP_BUF_SIZE - n,
						      "ENC2:");
					n += hex_dump_line(
						mon->resp_buf + n,
						RESP_BUF_SIZE - n,
						e->chat_enc + 16, enc2);
					n += snprintf(mon->resp_buf + n,
						      RESP_BUF_SIZE - n,
						      "\n");
				}
			}
		} else if (e->payload_len > 0) {
			n += snprintf(mon->resp_buf + n, RESP_BUF_SIZE - n,
				      "DATA:");
			n += hex_dump_line(mon->resp_buf + n,
					   RESP_BUF_SIZE - n,
					   e->payload, e->payload_len);
			n += snprintf(mon->resp_buf + n, RESP_BUF_SIZE - n,
				      "\n");
		}
	}
	spin_unlock_irqrestore(&mon->ring_lock, flags);

	if (count == 0)
		n += snprintf(mon->resp_buf + n, RESP_BUF_SIZE - n,
			      "\n  (empty - enable monitoring first: "
			      "'monitor on')\n");

	n += snprintf(mon->resp_buf + n, RESP_BUF_SIZE - n,
		      "TOTAL:%d\n", count);

	mon->resp_len = n;
	mon->resp_ready = true;
	mutex_unlock(&mon->cmd_lock);
	wake_up_interruptible(&mon->resp_wq);
}

/* ================================================================
 * Character device: /dev/rtl8188
 * ================================================================ */

static int rtl8188_dev_open(struct inode *inode, struct file *f)
{
	return 0;
}

static int rtl8188_dev_release(struct inode *inode, struct file *f)
{
	return 0;
}

static ssize_t rtl8188_dev_read(struct file *f, char __user *buf,
				size_t len, loff_t *off)
{
	struct rtl8188_mon *mon = g_mon;
	int avail, ret;

	if (!mon)
		return -ENODEV;

	if (!mon->resp_ready) {
		if (f->f_flags & O_NONBLOCK)
			return -EAGAIN;
		ret = wait_event_interruptible_timeout(
			mon->resp_wq, mon->resp_ready,
			msecs_to_jiffies(20000));
		if (ret == 0)
			return -ETIMEDOUT;
		if (ret < 0)
			return ret;
	}

	mutex_lock(&mon->cmd_lock);
	avail = mon->resp_len - (int)*off;
	if (avail <= 0) {
		mutex_unlock(&mon->cmd_lock);
		return 0;
	}
	ret = min_t(int, (int)len, avail);
	if (copy_to_user(buf, mon->resp_buf + *off, ret)) {
		mutex_unlock(&mon->cmd_lock);
		return -EFAULT;
	}
	*off += ret;
	mutex_unlock(&mon->cmd_lock);
	return ret;
}

static ssize_t rtl8188_dev_write(struct file *f, const char __user *buf,
				 size_t len, loff_t *off)
{
	struct rtl8188_mon *mon = g_mon;
	char cmd[MAX_CMD_LEN];
	int n;

	if (!mon)
		return -ENODEV;

	n = min_t(int, (int)len, MAX_CMD_LEN - 1);
	if (copy_from_user(cmd, buf, n))
		return -EFAULT;
	cmd[n] = '\0';

	while (n > 0 && (cmd[n - 1] == '\n' || cmd[n - 1] == '\r'))
		cmd[--n] = '\0';

	/* Invalidate previous response so read() blocks for new result */
	mutex_lock(&mon->cmd_lock);
	mon->resp_ready = false;
	mon->resp_len = 0;
	mutex_unlock(&mon->cmd_lock);

	if (strcmp(cmd, "scan") == 0) {
		queue_work(mon->wq, &mon->scan_work);

	} else if (strncmp(cmd, "connect ", 8) == 0) {
		char *args = cmd + 8;
		char *sp = strchr(args, ' ');

		if (sp) {
			*sp = '\0';
			strscpy(mon->cmd_ssid, args, sizeof(mon->cmd_ssid));
			strscpy(mon->cmd_pass, sp + 1,
				sizeof(mon->cmd_pass));
		} else {
			strscpy(mon->cmd_ssid, args, sizeof(mon->cmd_ssid));
			mon->cmd_pass[0] = '\0';
		}
		sanitize(mon->cmd_ssid, sizeof(mon->cmd_ssid));
		sanitize(mon->cmd_pass, sizeof(mon->cmd_pass));
		queue_work(mon->wq, &mon->connect_work);

	} else if (strcmp(cmd, "disconnect") == 0) {
		queue_work(mon->wq, &mon->disconnect_work);

	} else if (strcmp(cmd, "monitor on") == 0) {
		if (!mon->pkt_registered && mon->ndev) {
			mon->ptype.type = cpu_to_be16(ETH_P_ALL);
			mon->ptype.func = rtl8188_pkt_recv;
			mon->ptype.dev = mon->ndev;
			dev_add_pack(&mon->ptype);
			mon->pkt_registered = true;
		}
		mon->capture_on = true;
		set_resp(mon, "Packet monitoring: ON\n");

	} else if (strcmp(cmd, "monitor off") == 0) {
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
		mon->filter_port = htons(CHAT_PORT);
		atomic_set(&mon->filter_cnt, 0);
		set_resp(mon, "Filter: port 9090 (CryptoChat) only\n");

	} else if (strncmp(cmd, "filter ", 7) == 0) {
		unsigned int port;

		if (kstrtouint(cmd + 7, 10, &port) == 0 && port <= 65535) {
			mon->filter_port = htons(port);
			atomic_set(&mon->filter_cnt, 0);
			if (port == 0) {
				set_resp(mon,
					 "Filter: disabled (capture all)\n");
			} else {
				char tmp[80];
				snprintf(tmp, sizeof(tmp),
					 "Filter: port %u only\n", port);
				set_resp(mon, tmp);
			}
		} else {
			set_resp(mon,
				 "Usage: filter <port> or filter chat\n");
		}

	} else if (strcmp(cmd, "filter") == 0) {
		char tmp[80];

		if (mon->filter_port)
			snprintf(tmp, sizeof(tmp),
				 "Current filter: port %d\n",
				 ntohs(mon->filter_port));
		else
			snprintf(tmp, sizeof(tmp),
				 "No filter active (capturing all)\n");
		set_resp(mon, tmp);

	} else {
		set_resp(mon,
			 "============================================\n"
			 "        RTL8188 Monitor - Commands\n"
			 "============================================\n\n"
			 "  scan              Scan for WiFi networks\n"
			 "  connect SSID [PW] Connect to network\n"
			 "  disconnect        Disconnect\n"
			 "  status            Connection status\n"
			 "  info              Device information\n"
			 "  stats             Packet statistics\n"
			 "  monitor on|off    Packet monitoring\n"
			 "  capture           Show captured packets\n"
			 "  filter <port>     Filter by port number\n"
			 "  filter chat       Filter CryptoChat (9090)\n"
			 "  filter 0          Remove filter\n");
	}

	/* Reset file position so next read starts from 0 */
	*off = 0;
	return len;
}

static const struct file_operations rtl8188_fops = {
	.owner   = THIS_MODULE,
	.open    = rtl8188_dev_open,
	.release = rtl8188_dev_release,
	.read    = rtl8188_dev_read,
	.write   = rtl8188_dev_write,
};

/* ================================================================
 * /proc/rtl8188/ entries
 * ================================================================ */

static int proc_device_show(struct seq_file *m, void *v)
{
	struct rtl8188_mon *mon = g_mon;

	if (!mon)
		return 0;

	seq_puts(m, "=== RTL8188ETV USB WiFi Device ===\n\n");

	if (mon->dev_present && mon->udev) {
		struct usb_device *u = mon->udev;

		seq_printf(m, "Vendor:     0x%04x (%s)\n",
			   le16_to_cpu(u->descriptor.idVendor),
			   u->manufacturer ? u->manufacturer : "Realtek");
		seq_printf(m, "Product:    0x%04x (%s)\n",
			   le16_to_cpu(u->descriptor.idProduct),
			   u->product ? u->product : "RTL8188ETV");
		seq_printf(m, "USB Speed:  %s\n",
			   usb_speed_string(u->speed));
		seq_printf(m, "Interface:  %s\n",
			   mon->ifname[0] ? mon->ifname : "(none)");
		if (mon->ndev)
			seq_printf(m, "MAC:        %pM\n",
				   mon->ndev->dev_addr);
	} else {
		seq_puts(m, "Status: Device not found\n");
	}
	return 0;
}

static int proc_stats_show(struct seq_file *m, void *v)
{
	struct rtl8188_mon *mon = g_mon;
	struct rtnl_link_stats64 ns;

	if (!mon)
		return 0;

	seq_puts(m, "=== Packet Statistics ===\n\n");
	seq_printf(m, "Monitor: %s  |  Capture: %s\n\n",
		   mon->pkt_registered ? "ON" : "OFF",
		   mon->capture_on ? "ON" : "OFF");

	if (mon->pkt_registered) {
		seq_printf(m, "Monitored RX: %lld pkts, %lld bytes\n",
			   atomic64_read(&mon->rx_pkts),
			   atomic64_read(&mon->rx_bytes));
		seq_printf(m, "  ARP=%d  IPv4=%d  IPv6=%d  Other=%d\n",
			   atomic_read(&mon->arp_cnt),
			   atomic_read(&mon->ip_cnt),
			   atomic_read(&mon->ipv6_cnt),
			   atomic_read(&mon->other_cnt));
		seq_printf(m, "  TCP=%d  UDP=%d  ICMP=%d  Chat=%d\n\n",
			   atomic_read(&mon->tcp_cnt),
			   atomic_read(&mon->udp_cnt),
			   atomic_read(&mon->icmp_cnt),
			   atomic_read(&mon->chat_cnt));
	}

	if (mon->ndev) {
		dev_get_stats(mon->ndev, &ns);
		seq_printf(m, "Driver TX: %llu pkts, %llu bytes\n",
			   ns.tx_packets, ns.tx_bytes);
		seq_printf(m, "Driver RX: %llu pkts, %llu bytes\n",
			   ns.rx_packets, ns.rx_bytes);
	}

	seq_printf(m, "\nUptime: %lu seconds\n",
		   (jiffies - mon->load_jiffies) / HZ);
	return 0;
}

static int proc_status_show(struct seq_file *m, void *v)
{
	struct rtl8188_mon *mon = g_mon;
	char cmd[256];
	char *buf;
	int n;

	if (!mon || !mon->ifname[0]) {
		seq_puts(m, "No interface\n");
		return 0;
	}

	buf = kmalloc(4096, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	snprintf(cmd, sizeof(cmd),
		 "/usr/sbin/iw dev %s link > " STATUS_FILE " 2>&1", mon->ifname);
	run_cmd(cmd);
	n = read_tmpfile(STATUS_FILE, buf, 4096);
	if (n > 0)
		seq_printf(m, "%s", buf);
	else
		seq_puts(m, "Not connected\n");

	kfree(buf);
	return 0;
}

static int proc_scan_show(struct seq_file *m, void *v)
{
	struct rtl8188_mon *mon = g_mon;

	if (!mon)
		return 0;

	mutex_lock(&mon->cmd_lock);
	if (mon->resp_len > 0)
		seq_printf(m, "%s", mon->resp_buf);
	else
		seq_puts(m, "No scan results. Run: echo scan > /dev/rtl8188\n");
	mutex_unlock(&mon->cmd_lock);
	return 0;
}

/* ================================================================
 * Module init / exit
 * ================================================================ */

static int __init rtl8188_mon_init(void)
{
	int ret;

	g_mon = kzalloc(sizeof(*g_mon), GFP_KERNEL);
	if (!g_mon)
		return -ENOMEM;

	g_mon->resp_buf = kzalloc(RESP_BUF_SIZE, GFP_KERNEL);
	if (!g_mon->resp_buf) {
		ret = -ENOMEM;
		goto err_free;
	}

	mutex_init(&g_mon->cmd_lock);
	spin_lock_init(&g_mon->ring_lock);
	init_waitqueue_head(&g_mon->resp_wq);
	INIT_WORK(&g_mon->scan_work, scan_work_fn);
	INIT_WORK(&g_mon->connect_work, connect_work_fn);
	INIT_WORK(&g_mon->disconnect_work, disconnect_work_fn);
	g_mon->load_jiffies = jiffies;

	/* Workqueue for async commands */
	g_mon->wq = create_singlethread_workqueue("rtl8188_mon");
	if (!g_mon->wq) {
		ret = -ENOMEM;
		goto err_buf;
	}

	/* Character device */
	ret = alloc_chrdev_region(&g_mon->devno, 0, 1, DEVICE_NAME);
	if (ret)
		goto err_wq;

	cdev_init(&g_mon->cdev, &rtl8188_fops);
	g_mon->cdev.owner = THIS_MODULE;
	ret = cdev_add(&g_mon->cdev, g_mon->devno, 1);
	if (ret)
		goto err_region;

	g_mon->cls = class_create(DEVICE_NAME);
	if (IS_ERR(g_mon->cls)) {
		ret = PTR_ERR(g_mon->cls);
		goto err_cdev;
	}

	g_mon->chrdev = device_create(g_mon->cls, NULL, g_mon->devno,
				      NULL, DEVICE_NAME);
	if (IS_ERR(g_mon->chrdev)) {
		ret = PTR_ERR(g_mon->chrdev);
		goto err_class;
	}

	/* Proc entries */
	g_mon->proc_dir = proc_mkdir(DEVICE_NAME, NULL);
	if (g_mon->proc_dir) {
		proc_create_single("device", 0444, g_mon->proc_dir,
				   proc_device_show);
		proc_create_single("stats", 0444, g_mon->proc_dir,
				   proc_stats_show);
		proc_create_single("status", 0444, g_mon->proc_dir,
				   proc_status_show);
		proc_create_single("scan", 0444, g_mon->proc_dir,
				   proc_scan_show);
	}

	/* USB notifier */
	g_mon->usb_nb.notifier_call = rtl8188_usb_notify;
	usb_register_notify(&g_mon->usb_nb);

	/* Netdev notifier */
	g_mon->net_nb.notifier_call = rtl8188_net_notify;
	register_netdevice_notifier(&g_mon->net_nb);

	/* Detect already-present hardware */
	scan_existing_usb();
	if (g_mon->dev_present)
		scan_existing_netdev();

	pr_info("[rtl8188_mon] Companion monitor loaded. "
		"Device: /dev/" DEVICE_NAME "  Proc: /proc/" DEVICE_NAME "/\n");
	if (g_mon->dev_present)
		pr_info("[rtl8188_mon] RTL8188ETV found, interface: %s\n",
			g_mon->ifname[0] ? g_mon->ifname : "(pending)");

	return 0;

err_class:
	class_destroy(g_mon->cls);
err_cdev:
	cdev_del(&g_mon->cdev);
err_region:
	unregister_chrdev_region(g_mon->devno, 1);
err_wq:
	destroy_workqueue(g_mon->wq);
err_buf:
	kfree(g_mon->resp_buf);
err_free:
	kfree(g_mon);
	g_mon = NULL;
	return ret;
}

static void __exit rtl8188_mon_exit(void)
{
	if (!g_mon)
		return;

	/* Stop packet monitoring */
	if (g_mon->pkt_registered)
		dev_remove_pack(&g_mon->ptype);

	/* Drain async work */
	cancel_work_sync(&g_mon->scan_work);
	cancel_work_sync(&g_mon->connect_work);
	cancel_work_sync(&g_mon->disconnect_work);
	destroy_workqueue(g_mon->wq);

	/* Notifiers */
	unregister_netdevice_notifier(&g_mon->net_nb);
	usb_unregister_notify(&g_mon->usb_nb);

	/* Proc */
	if (g_mon->proc_dir) {
		remove_proc_entry("device", g_mon->proc_dir);
		remove_proc_entry("stats", g_mon->proc_dir);
		remove_proc_entry("status", g_mon->proc_dir);
		remove_proc_entry("scan", g_mon->proc_dir);
		remove_proc_entry(DEVICE_NAME, NULL);
	}

	/* Char device */
	device_destroy(g_mon->cls, g_mon->devno);
	class_destroy(g_mon->cls);
	cdev_del(&g_mon->cdev);
	unregister_chrdev_region(g_mon->devno, 1);

	/* USB ref */
	if (g_mon->udev)
		usb_put_dev(g_mon->udev);

	/* Cleanup temp files (fire-and-forget) */
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
