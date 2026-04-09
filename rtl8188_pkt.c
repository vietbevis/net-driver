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
#include <linux/ctype.h>

/* ================================================================
 * Domain name helpers (DNS cache + HTTP Host + TLS SNI)
 * ================================================================ */

/* Very small IPv4→domain cache for display purposes */
#define DNS_CACHE_MAX 128

struct dns_cache_ent {
	__be32 ip;
	char   name[80];
	unsigned long updated_j;
};

static struct dns_cache_ent g_dns_cache[DNS_CACHE_MAX];
static spinlock_t g_dns_lock;

static void dns_cache_put(__be32 ip, const char *name)
{
	unsigned long flags;
	int i, empty = -1, oldest = 0;
	unsigned long oldest_j = ~0UL;

	if (!ip || !name || !name[0])
		return;

	spin_lock_irqsave(&g_dns_lock, flags);
	for (i = 0; i < DNS_CACHE_MAX; i++) {
		if (g_dns_cache[i].ip == ip) {
			strscpy(g_dns_cache[i].name, name, sizeof(g_dns_cache[i].name));
			g_dns_cache[i].updated_j = jiffies;
			spin_unlock_irqrestore(&g_dns_lock, flags);
			return;
		}
		if (!g_dns_cache[i].ip && empty < 0)
			empty = i;
		if (time_before(g_dns_cache[i].updated_j, oldest_j)) {
			oldest_j = g_dns_cache[i].updated_j;
			oldest = i;
		}
	}

	i = (empty >= 0) ? empty : oldest;
	g_dns_cache[i].ip = ip;
	strscpy(g_dns_cache[i].name, name, sizeof(g_dns_cache[i].name));
	g_dns_cache[i].updated_j = jiffies;
	spin_unlock_irqrestore(&g_dns_lock, flags);
}

static const char *dns_cache_get(__be32 ip, char out[80])
{
	unsigned long flags;
	int i;

	if (!ip)
		return NULL;

	spin_lock_irqsave(&g_dns_lock, flags);
	for (i = 0; i < DNS_CACHE_MAX; i++) {
		if (g_dns_cache[i].ip == ip && g_dns_cache[i].name[0]) {
			strscpy(out, g_dns_cache[i].name, 80);
			spin_unlock_irqrestore(&g_dns_lock, flags);
			return out;
		}
	}
	spin_unlock_irqrestore(&g_dns_lock, flags);
	return NULL;
}

/* DNS name decode with compression pointers — best-effort, safe */
static int dns_read_name(const u8 *pkt, int pkt_len, int off,
			 char *out, int out_sz)
{
	int n = 0;
	int jumped = 0;
	int jump_off = -1;
	int depth = 0;

	if (out && out_sz > 0)
		out[0] = '\0';

	while (off < pkt_len && depth++ < 32) {
		u8 lab = pkt[off];
		int i;

		if (lab == 0) {
			off++;
			if (n == 0) {
				if (out && out_sz > 1) {
					out[0] = '.';
					out[1] = '\0';
				}
			} else if (n < out_sz) {
				if (out) out[n] = '\0';
			}
			return jumped && jump_off >= 0 ? jump_off : off;
		}

		/* compression pointer */
		if ((lab & 0xC0) == 0xC0) {
			int ptr;
			if (off + 1 >= pkt_len)
				return -1;
			ptr = ((pkt[off] & 0x3F) << 8) | pkt[off + 1];
			if (!jumped) {
				jumped = 1;
				jump_off = off + 2;
			}
			if (ptr < 0 || ptr >= pkt_len)
				return -1;
			off = ptr;
			continue;
		}

		/* invalid label length */
		if (lab > 63)
			return -1;

		if (out && n && n < out_sz - 1)
			out[n++] = '.';

		off++;
		if (off + lab > pkt_len)
			return -1;
		for (i = 0; i < lab && (!out || n < out_sz - 1); i++) {
			u8 c = pkt[off + i];
			if (out)
				out[n++] = (c >= 32 && c <= 126) ? (char)c : '_';
			else
				n++;
		}
		/* skip remaining label bytes if truncated */
		if (i < lab)
			off += lab;
		else
			off += lab;
	}
	return -1;
}

static void try_parse_dns_response_ipv4_map(struct pkt_entry *e)
{
	/* Only UDP/53 responses (best-effort) */
	const u8 *p = e->payload;
	int len = e->payload_len;
	u16 flags, qd, an;
	int off, qname_end, i;
	char qname[80];

	if (len < 12)
		return;
	if (!(ntohs(e->src_port) == 53 || ntohs(e->dst_port) == 53))
		return;

	flags = (p[2] << 8) | p[3];
	/* QR bit must be 1 for response */
	if ((flags & 0x8000) == 0)
		return;

	qd = (p[4] << 8) | p[5];
	an = (p[6] << 8) | p[7];
	if (qd == 0 || an == 0)
		return;

	off = 12;
	memset(qname, 0, sizeof(qname));
	qname_end = dns_read_name(p, len, off, qname, sizeof(qname));
	if (qname_end < 0)
		return;
	off = qname_end;
	if (off + 4 > len)
		return;
	off += 4; /* QTYPE+QCLASS */

	/* Answers: only handle NAME as root pointer-less (skip) */
	for (i = 0; i < an && off + 10 <= len; i++) {
		int name_end;
		u16 type, rdlen;

		name_end = dns_read_name(p, len, off, NULL, 0);
		if (name_end < 0)
			break;
		off = name_end;
		if (off + 10 > len)
			break;

		type = (p[off] << 8) | p[off + 1];
		rdlen = (p[off + 8] << 8) | p[off + 9];
		off += 10;
		if (off + rdlen > len)
			break;

		if (type == 1 && rdlen == 4) { /* A */
			__be32 ip = *(__be32 *)(p + off);
			dns_cache_put(ip, qname);
		}
		off += rdlen;
	}
}

static void try_parse_http_host(struct pkt_entry *e, __be32 dst_ip)
{
	const u8 *p = e->payload;
	int len = e->payload_len;
	int i;

	/* Heuristic: only for TCP to 80/8080 and printable payload */
	if (e->ip_proto != IPPROTO_TCP)
		return;
	if (!(ntohs(e->dst_port) == 80 || ntohs(e->dst_port) == 8080))
		return;
	if (len < 16)
		return;

	for (i = 0; i + 6 < len; i++) {
		if ((p[i] == 'H' || p[i] == 'h') &&
		    (p[i+1] == 'o' || p[i+1] == 'O') &&
		    (p[i+2] == 's' || p[i+2] == 'S') &&
		    (p[i+3] == 't' || p[i+3] == 'T') &&
		    p[i+4] == ':' ) {
			int j = i + 5;
			int n = 0;
			while (j < len && (p[j] == ' ' || p[j] == '\t')) j++;
			while (j < len && p[j] != '\r' && p[j] != '\n' &&
			       n < (int)sizeof(e->domain) - 1) {
				u8 c = p[j++];
				if (c < 32 || c > 126) break;
				e->domain[n++] = (char)c;
			}
			e->domain[n] = '\0';
			if (e->domain[0]) {
				dns_cache_put(dst_ip, e->domain);
			}
			return;
		}
	}
}

/* Minimal TLS ClientHello SNI extraction for HTTPS */
static void try_parse_tls_sni(struct pkt_entry *e, __be32 dst_ip)
{
	const u8 *p = e->payload;
	int len = e->payload_len;
	int off, hs_off, ext_off, exts_len;

	if (e->ip_proto != IPPROTO_TCP)
		return;
	if (ntohs(e->dst_port) != 443)
		return;
	if (len < 60)
		return;
	/* TLS record header */
	if (p[0] != 0x16) /* Handshake */
		return;
	/* handshake message should start at 5 */
	hs_off = 5;
	if (hs_off + 4 > len)
		return;
	if (p[hs_off] != 0x01) /* ClientHello */
		return;
	off = hs_off + 4;
	/* client_version + random */
	if (off + 2 + 32 > len)
		return;
	off += 2 + 32;
	/* session id */
	if (off + 1 > len) return;
	off += 1 + p[off];
	/* cipher suites */
	if (off + 2 > len) return;
	off += 2 + ((p[off] << 8) | p[off + 1]);
	/* compression methods */
	if (off + 1 > len) return;
	off += 1 + p[off];
	/* extensions */
	if (off + 2 > len) return;
	exts_len = (p[off] << 8) | p[off + 1];
	off += 2;
	ext_off = off;
	if (ext_off + exts_len > len)
		return;

	while (off + 4 <= ext_off + exts_len) {
		u16 etype = (p[off] << 8) | p[off + 1];
		u16 elen  = (p[off + 2] << 8) | p[off + 3];
		off += 4;
		if (off + elen > ext_off + exts_len)
			return;
		if (etype == 0x0000 && elen >= 5) { /* server_name */
			int s = off;
			int list_len = (p[s] << 8) | p[s + 1];
			s += 2;
			if (s + list_len > off + elen)
				return;
			/* first entry */
			if (s + 3 > off + elen) return;
			if (p[s] != 0x00) return; /* host_name */
			{
				int name_len = (p[s + 1] << 8) | p[s + 2];
				int n = 0;
				s += 3;
				if (s + name_len > off + elen)
					return;
				while (n < (int)sizeof(e->domain) - 1 && n < name_len) {
					u8 c = p[s + n];
					if (c < 32 || c > 126) break;
					e->domain[n] = (char)c;
					n++;
				}
				e->domain[n] = '\0';
				if (e->domain[0])
					dns_cache_put(dst_ip, e->domain);
			}
			return;
		}
		off += elen;
	}
}

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

	/* Domain best-effort */
	try_parse_dns_response_ipv4_map(e);
	if (iph && !e->domain[0]) {
		__be32 sip = iph->saddr;
		__be32 dip = iph->daddr;
		__be32 server_ip = dip;

		/* If packet is inbound from server (src_port is 443/80), map by src_ip */
		if (e->ip_proto == IPPROTO_TCP &&
		    (ntohs(e->src_port) == 443 || ntohs(e->src_port) == 80 ||
		     ntohs(e->src_port) == 8080))
			server_ip = sip;

		try_parse_tls_sni(e, server_ip);
		if (!e->domain[0])
			try_parse_http_host(e, server_ip);
		if (!e->domain[0]) {
			char tmpdom[80];
			if (dns_cache_get(server_ip, tmpdom))
				strscpy(e->domain, tmpdom, sizeof(e->domain));
		}
	}

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