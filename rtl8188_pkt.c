// SPDX-License-Identifier: GPL-2.0
/*
 * RTL8188ETV WiFi Companion Monitor — Packet Monitoring
 *
 * Đăng ký packet handler qua dev_add_pack(ETH_P_ALL) để nhận bản sao
 * (clone) của tất cả gói tin đi qua wireless interface.
 *
 * Pipeline xử lý mỗi gói tin (rtl8188_pkt_recv):
 *   1. Tăng bộ đếm L2 (ARP/IPv4/IPv6/Other)
 *   2. Nếu capture_on: parse IPv4 header → lấy L4 proto, src/dst port
 *   3. Snapshot tối đa PAYLOAD_SNAP_SIZE byte payload
 *   4. Phát hiện CryptoChat qua port 9090 hoặc nội dung chat_frame
 *   5. Áp dụng port filter (nếu có): bỏ qua gói không khớp
 *   6. Lưu vào ring buffer (circular, 128 entry)
 *   7. kfree_skb() — giải phóng clone
 *
 * Ring buffer được bảo vệ bởi spinlock với irqsave vì handler
 * chạy trong softirq context.
 */

#include "rtl8188_mon.h"
#include <linux/skbuff.h>
#include <linux/etherdevice.h>
#include <linux/ip.h>
#include <linux/ipv6.h>
#include <linux/tcp.h>
#include <linux/udp.h>

/* ================================================================
 * Hàm phụ trợ phân tích chat_frame
 * ================================================================ */

/**
 * try_parse_chat_frame() - Thử phân tích payload như một chat_frame
 * @e: Entry trong ring buffer để điền thông tin (nếu hợp lệ)
 *
 * chat_frame hợp lệ phải có:
 *   - byte đầu (version) == 1
 *   - byte thứ hai (type) trong khoảng MSG_TYPE_AUTH..MSG_TYPE_REG_FAIL
 *   - payload_len >= CF_HDR_SIZE (52 byte)
 *
 * Nếu hợp lệ, trích xuất: IV (16B), HMAC (32B), 32B đầu ciphertext.
 * Hàm này hỗ trợ phát hiện chat_frame trên bất kỳ cổng nào (không chỉ 9090).
 */
static void try_parse_chat_frame(struct pkt_entry *e)
{
	const u8 *f;
	u8 ver, type;

	/* Cần đủ dữ liệu để đọc header */
	if (e->payload_len < CF_HDR_SIZE)
		return;

	f    = e->payload;
	ver  = f[CF_OFF_VER];
	type = f[CF_OFF_TYPE];

	/* Kiểm tra magic: version=1 và type hợp lệ */
	if (ver != 1 || type < MSG_TYPE_AUTH || type > MSG_TYPE_REG_FAIL)
		return;

	/* Điền thông tin header */
	e->chat_ver  = ver;
	e->chat_type = type;
	/* plen lưu ở little-endian trong wire format */
	e->chat_plen = f[CF_OFF_PLEN] | ((u16)f[CF_OFF_PLEN + 1] << 8);

	/* Sao chép IV (16 byte) */
	memcpy(e->chat_iv,   f + CF_OFF_IV,   16);

	/* Sao chép HMAC-SHA256 (32 byte) */
	memcpy(e->chat_hmac, f + CF_OFF_HMAC, 32);

	/* Sao chép tối đa 32 byte đầu của ciphertext */
	e->chat_enc_len = e->payload_len - CF_HDR_SIZE;
	if (e->chat_enc_len > 32)
		e->chat_enc_len = 32;
	if (e->chat_enc_len > 0)
		memcpy(e->chat_enc, f + CF_OFF_PAYLOAD, e->chat_enc_len);

	e->chat_parsed = 1;

	/*
	 * Nếu phát hiện chat_frame trên cổng không phải 9090,
	 * vẫn đánh dấu và đếm vào chat_cnt.
	 */
	if (!e->is_chat) {
		e->is_chat = 1;
		atomic_inc(&g_mon->chat_cnt);
	}
}

/* ================================================================
 * Packet handler chính
 * ================================================================ */

/**
 * rtl8188_pkt_recv() - Handler nhận gói tin từ Linux networking stack
 * @skb:      Socket buffer chứa gói tin (đã là clone, ta phải kfree_skb)
 * @dev:      Interface mà gói tin đến (đã được kernel điền sẵn)
 * @pt:       Packet type structure (con trỏ tới g_mon->ptype)
 * @orig_dev: Interface gốc (trước khi bond/vlan, thường giống dev)
 *
 * Được đăng ký qua dev_add_pack() với ETH_P_ALL — nhận tất cả gói tin.
 * Chạy trong softirq context: không được sleep, không được alloc với GFP_KERNEL.
 *
 * Trả về: 0 (luôn, theo convention của packet_type.func)
 */
int rtl8188_pkt_recv(struct sk_buff *skb, struct net_device *dev,
		     struct packet_type *pt, struct net_device *orig_dev)
{
	unsigned long flags;
	struct pkt_entry *e;
	struct iphdr *iph = NULL;
	struct tcphdr *th;
	struct udphdr *uh;
	__be16 sport = 0, dport = 0;
	u8 l4_proto = 0;
	const u8 *l4_payload     = NULL;
	int       l4_payload_len  = 0;
	int       is_chat         = 0;

	/* Chỉ xử lý gói của interface đang monitor */
	if (!g_mon || dev != g_mon->ndev)
		goto out;

	/* Cập nhật bộ đếm tổng — luôn chạy, không phụ thuộc capture_on */
	atomic64_inc(&g_mon->rx_pkts);
	atomic64_add(skb->len, &g_mon->rx_bytes);

	/* Phân loại L2 protocol */
	switch (ntohs(skb->protocol)) {
	case ETH_P_ARP:  atomic_inc(&g_mon->arp_cnt);   break;
	case ETH_P_IP:   atomic_inc(&g_mon->ip_cnt);    break;
	case ETH_P_IPV6: atomic_inc(&g_mon->ipv6_cnt);  break;
	default:         atomic_inc(&g_mon->other_cnt); break;
	}

	/* Chỉ làm deep inspection khi capture đang bật */
	if (!g_mon->capture_on)
		goto out;

	/* --- Phân tích IPv4 --- */
	if (ntohs(skb->protocol) == ETH_P_IP &&
	    skb->len >= sizeof(struct iphdr)) {
		iph      = ip_hdr(skb);
		l4_proto = iph->protocol;

		if (l4_proto == IPPROTO_TCP &&
		    skb->len >= (unsigned int)(iph->ihl * 4 + sizeof(struct tcphdr))) {
			atomic_inc(&g_mon->tcp_cnt);
			th     = (struct tcphdr *)((u8 *)iph + iph->ihl * 4);
			sport  = th->source;
			dport  = th->dest;
			/* Payload TCP bắt đầu sau TCP header */
			l4_payload     = (u8 *)th + th->doff * 4;
			l4_payload_len = skb->len - iph->ihl * 4 - th->doff * 4;
			if (l4_payload_len < 0)
				l4_payload_len = 0;

		} else if (l4_proto == IPPROTO_UDP &&
			   skb->len >= (unsigned int)(iph->ihl * 4 + sizeof(struct udphdr))) {
			atomic_inc(&g_mon->udp_cnt);
			uh     = (struct udphdr *)((u8 *)iph + iph->ihl * 4);
			sport  = uh->source;
			dport  = uh->dest;
			/* Payload UDP bắt đầu sau UDP header (8 byte) */
			l4_payload     = (u8 *)uh + sizeof(struct udphdr);
			l4_payload_len = ntohs(uh->len) - sizeof(struct udphdr);
			if (l4_payload_len < 0)
				l4_payload_len = 0;

		} else if (l4_proto == IPPROTO_ICMP) {
			atomic_inc(&g_mon->icmp_cnt);
		}

		/* Phát hiện CryptoChat theo cổng mặc định */
		if (ntohs(sport) == CHAT_PORT || ntohs(dport) == CHAT_PORT) {
			is_chat = 1;
			atomic_inc(&g_mon->chat_cnt);
		}
	}

	/* Đếm gói khớp filter port (luôn đếm, kể cả khi gói bị bỏ qua bên dưới) */
	if (g_mon->filter_port != 0 &&
	    (sport == g_mon->filter_port || dport == g_mon->filter_port))
		atomic_inc(&g_mon->filter_cnt);

	/*
	 * Áp dụng port filter cho ring buffer:
	 * Nếu filter_port != 0, chỉ lưu gói khớp với cổng đó.
	 */
	if (g_mon->filter_port != 0 &&
	    sport != g_mon->filter_port &&
	    dport != g_mon->filter_port)
		goto out;

	/* --- Ghi vào ring buffer --- */
	spin_lock_irqsave(&g_mon->ring_lock, flags);

	e = &g_mon->ring[g_mon->ring_head];
	memset(e, 0, sizeof(*e));

	/* Metadata cơ bản */
	e->tstamp    = jiffies;
	e->eth_proto = skb->protocol;
	e->len       = skb->len;
	e->ip_proto  = l4_proto;
	e->src_port  = sport;
	e->dst_port  = dport;
	e->is_chat   = is_chat;

	/* Địa chỉ MAC (nếu Ethernet header còn trong skb) */
	if (skb_mac_header_was_set(skb)) {
		struct ethhdr *eth = eth_hdr(skb);
		memcpy(e->src_mac, eth->h_source, ETH_ALEN);
		memcpy(e->dst_mac, eth->h_dest,   ETH_ALEN);
	}

	/* Địa chỉ IP (chỉ hợp lệ nếu IPv4) */
	if (iph) {
		e->src_ip = iph->saddr;
		e->dst_ip = iph->daddr;
	}

	/* Snapshot payload: dùng skb_copy_bits để an toàn với paged skb */
	if (l4_payload && l4_payload_len > 0) {
		int snap = min_t(int, l4_payload_len, PAYLOAD_SNAP_SIZE);
		/*
		 * Tính offset từ đầu skb->data (network header) đến l4_payload.
		 * Cần thiết cho skb_copy_bits() vì l4_payload có thể trỏ
		 * vào dữ liệu nonlinear (paged).
		 */
		int off = (int)(l4_payload - (u8 *)ip_hdr(skb))
			  + skb_network_offset(skb);

		if (off >= 0 && off + snap <= (int)skb->len &&
		    skb_copy_bits(skb, off, e->payload, snap) == 0)
			e->payload_len = snap;
	}

	/* Thử phân tích cấu trúc chat_frame từ payload đã snapshot */
	try_parse_chat_frame(e);

	/* Cập nhật ring buffer theo kiểu circular (overwrite cũ nhất khi đầy) */
	g_mon->ring_head = (g_mon->ring_head + 1) % CAPTURE_RING_SIZE;
	if (g_mon->ring_count < CAPTURE_RING_SIZE)
		g_mon->ring_count++;

	spin_unlock_irqrestore(&g_mon->ring_lock, flags);

out:
	/* Luôn phải giải phóng clone skb — đây là trách nhiệm của packet handler */
	kfree_skb(skb);
	return 0;
}