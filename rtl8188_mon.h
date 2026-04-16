/* SPDX-License-Identifier: GPL-2.0 */
/*
 * RTL8188ETV WiFi Companion Monitor — Header dùng chung
 *
 * File này định nghĩa tất cả hằng số, cấu trúc dữ liệu và khai báo extern
 * được chia sẻ giữa các file nguồn của kernel module.
 */

#ifndef RTL8188_MON_H
#define RTL8188_MON_H

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/usb.h>
#include <linux/netdevice.h>
#include <linux/if_ether.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/proc_fs.h>
#include <linux/workqueue.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/wait.h>
#include <linux/ip.h>
#include <linux/tcp.h>
#include <linux/udp.h>

/* ================================================================
 * Hằng số định danh thiết bị và cấu hình
 * ================================================================ */

/* Tên device node: /dev/rtl8188 */
#define DEVICE_NAME       "rtl8188"

/* USB Vendor ID và Product ID của RTL8188ETV */
#define TARGET_VENDOR     0x0bda
#define TARGET_PRODUCT    0x0179

/* Kích thước buffer lệnh/phản hồi: 32 KB */
#define RESP_BUF_SIZE     (32 * 1024)

/* Số lượng entry tối đa trong ring buffer bắt gói */
#define CAPTURE_RING_SIZE 128

/* Độ dài lệnh tối đa từ userspace */
#define MAX_CMD_LEN       256

/*
 * Kích thước snapshot payload:
 * Đủ để chứa toàn bộ chat_frame header (52 byte) + 76 byte payload mã hóa
 */
#define PAYLOAD_SNAP_SIZE 1280

/* Cổng TCP mà ứng dụng CryptoChat sử dụng */
#define CHAT_PORT         9090

/* Đường dẫn file tạm lưu kết quả từ userspace helper */
#define SCAN_FILE         "/tmp/.rtl8188_scan"
#define STATUS_FILE       "/tmp/.rtl8188_status"
#define CONNECT_FILE      "/tmp/.rtl8188_connect"

/* ================================================================
 * Định nghĩa wire format của chat_frame (phải khớp với crypto_chat.h)
 * ================================================================ */

/* Kích thước phần header cố định của một chat_frame */
#define CF_HDR_SIZE    52   /* 1(ver) + 1(type) + 2(plen) + 16(IV) + 32(HMAC) */

/* Offset các trường trong chat_frame */
#define CF_OFF_VER      0   /* 1 byte: phiên bản giao thức */
#define CF_OFF_TYPE     1   /* 1 byte: loại message */
#define CF_OFF_PLEN     2   /* 2 byte (u16 LE): độ dài payload mã hóa */
#define CF_OFF_IV       4   /* 16 byte: AES-CBC Initialization Vector */
#define CF_OFF_HMAC    20   /* 32 byte: HMAC-SHA256 xác thực toàn khung */
#define CF_OFF_PAYLOAD 52   /* N byte: payload đã mã hóa AES-256-CBC */

/* Các loại message được hỗ trợ */
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

/* ================================================================
 * Cấu trúc mô tả một gói tin đã bắt
 * ================================================================ */

/**
 * struct pkt_entry - Một entry trong ring buffer bắt gói tin
 *
 * Lưu trữ metadata và payload snapshot của một gói tin đã qua
 * deep packet inspection. Struct này được điền trong softirq context
 * (hàm rtl8188_pkt_recv), nên tất cả truy cập phải có ring_lock.
 */
struct pkt_entry {
	/* Thời điểm bắt gói (jiffies) */
	unsigned long tstamp;

	/* Địa chỉ MAC nguồn và đích (L2) */
	u8 src_mac[ETH_ALEN];
	u8 dst_mac[ETH_ALEN];

	/* EtherType (ETH_P_IP, ETH_P_ARP, ...) ở network byte order */
	__be16 eth_proto;

	/* Tổng kích thước gói tin (byte) */
	unsigned int len;

	/* Địa chỉ IP nguồn/đích ở network byte order (chỉ hợp lệ nếu IPv4) */
	__be32 src_ip;
	__be32 dst_ip;

	/* Giao thức L4: IPPROTO_TCP, IPPROTO_UDP, IPPROTO_ICMP, ... */
	u8 ip_proto;

	/* Cổng nguồn và đích ở network byte order */
	__be16 src_port;
	__be16 dst_port;

	/* Snapshot tối đa PAYLOAD_SNAP_SIZE byte đầu của L4 payload */
	u8  payload[PAYLOAD_SNAP_SIZE];
	int payload_len;

	/* Best-effort domain/host (DNS map / HTTP Host / TLS SNI). UTF-8 bytes. */
	char domain[80];

	/* 1 nếu src hoặc dst port là CHAT_PORT (9090) hoặc nội dung là chat_frame */
	int is_chat;

	/* --- Các trường sau chỉ hợp lệ khi chat_parsed == 1 --- */

	/* Đã phân tích thành công cấu trúc chat_frame chưa */
	int chat_parsed;

	/* Phiên bản giao thức từ chat_frame */
	u8  chat_ver;

	/* Loại message (MSG_TYPE_*) */
	u8  chat_type;

	/* Độ dài payload mã hóa khai báo trong frame */
	u16 chat_plen;

	/* AES Initialization Vector (16 byte) */
	u8  chat_iv[16];

	/* HMAC-SHA256 xác thực (32 byte) */
	u8  chat_hmac[32];

	/* Tối đa 32 byte đầu của payload đã mã hóa (ciphertext) */
	u8  chat_enc[32];
	int chat_enc_len;
};

/* ================================================================
 * Cấu trúc trạng thái toàn cục của module
 * ================================================================ */

/**
 * struct rtl8188_mon - Trạng thái chính của companion monitor module
 *
 * Một instance duy nhất được cấp phát khi module load (g_mon).
 * Tất cả dữ liệu chia sẻ giữa các file nguồn đều nằm ở đây.
 */
struct rtl8188_mon {
	/* ---- USB device ---- */

	/* Con trỏ tới USB device (lấy qua usb_get_dev, không claim interface) */
	struct usb_device *udev;

	/* true nếu thiết bị RTL8188ETV đang được cắm */
	bool dev_present;

	/* Notifier để nhận sự kiện USB plug/unplug */
	struct notifier_block usb_nb;

	/* ---- Network interface ---- */

	/* Con trỏ tới net_device của wlan interface thuộc về RTL8188ETV */
	struct net_device *ndev;

	/* Tên interface hiện tại (vd: "wlan0", "wlp10s0u4u1") */
	char ifname[IFNAMSIZ];

	/* Notifier để theo dõi thay đổi trạng thái network interface */
	struct notifier_block net_nb;

	/* ---- Packet monitoring ---- */

	/* Cấu trúc đăng ký packet handler với Linux networking stack */
	struct packet_type ptype;

	/* true nếu packet handler đã được đăng ký qua dev_add_pack() */
	bool pkt_registered;

	/* Bộ đếm số gói và byte đã nhận (lock-free, dùng mọi context) */
	atomic64_t rx_pkts;
	atomic64_t rx_bytes;

	/* Bộ đếm theo loại giao thức L2 */
	atomic_t arp_cnt;    /* ARP */
	atomic_t ip_cnt;     /* IPv4 */
	atomic_t ipv6_cnt;   /* IPv6 */
	atomic_t other_cnt;  /* Các loại khác */

	/* ---- Ring buffer bắt gói ---- */

	/* Mảng circular buffer lưu các gói đã bắt */
	struct pkt_entry ring[CAPTURE_RING_SIZE];

	/* Index entry tiếp theo sẽ được ghi (head của vòng tròn) */
	int ring_head;

	/* Số entry hợp lệ hiện tại trong ring (0 đến CAPTURE_RING_SIZE) */
	int ring_count;

	/*
	 * Spinlock bảo vệ ring buffer.
	 * Phải dùng irqsave vì packet handler chạy trong softirq context.
	 */
	spinlock_t ring_lock;

	/* true nếu đang ghi gói vào ring buffer */
	bool capture_on;

	/* Bộ đếm theo loại giao thức L4 */
	atomic_t tcp_cnt;   /* TCP */
	atomic_t udp_cnt;   /* UDP */
	atomic_t icmp_cnt;  /* ICMP */

	/* Đếm gói CryptoChat (port 9090 hoặc có chat_frame header hợp lệ) */
	atomic_t chat_cnt;

	/* Đếm gói khớp với filter_port đang active */
	atomic_t filter_cnt;

	/*
	 * Cổng port đang được lọc (network byte order).
	 * 0 = không lọc (bắt tất cả gói vào ring buffer).
	 */
	__be16 filter_port;

	/* ---- Command/Response buffer ---- */

	/*
	 * Mutex bảo vệ resp_buf, resp_len, resp_ready.
	 * Dùng trong process context (write/read syscall và workqueue).
	 */
	struct mutex cmd_lock;

	/* Buffer chứa kết quả lệnh để userspace read() lấy về */
	char *resp_buf;

	/* Số byte hợp lệ trong resp_buf */
	int resp_len;

	/* true khi resp_buf đã sẵn sàng để read() */
	bool resp_ready;

	/* Wait queue để read() chờ kết quả async (scan, connect, disconnect) */
	wait_queue_head_t resp_wq;

	/* ---- Async work (scan, connect, disconnect) ---- */

	/* Single-thread workqueue để serialize các tác vụ async */
	struct workqueue_struct *wq;

	struct work_struct scan_work;
	struct work_struct connect_work;
	struct work_struct disconnect_work;

	/* SSID và password cho lệnh connect hiện tại */
	char cmd_ssid[64];
	char cmd_pass[128];

	/* ---- Character device /dev/rtl8188 ---- */

	dev_t devno;
	struct cdev cdev;
	struct class *cls;
	struct device *chrdev;

	/* ---- Proc filesystem /proc/rtl8188/ ---- */

	struct proc_dir_entry *proc_dir;

	/* Thời điểm module được load (để tính uptime) */
	unsigned long load_jiffies;
};

/* ================================================================
 * Biến toàn cục duy nhất (định nghĩa trong rtl8188_main.c)
 * ================================================================ */

/* Con trỏ tới instance duy nhất của rtl8188_mon */
extern struct rtl8188_mon *g_mon;

/*
 * Mảng môi trường truyền vào call_usermodehelper().
 * Định nghĩa trong rtl8188_main.c, dùng chung ở rtl8188_cmd.c.
 */
extern char *helper_envp[];

/* ================================================================
 * Hàm tiện ích dùng chung (định nghĩa trong rtl8188_main.c)
 * ================================================================ */

/**
 * run_cmd() - Chạy một lệnh shell thông qua call_usermodehelper()
 * @cmd: Chuỗi lệnh shell (sẽ được truyền vào /bin/sh -c)
 *
 * Chạy đồng bộ (UMH_WAIT_PROC), block cho đến khi lệnh hoàn tất.
 * Lưu ý: SELinux phải ở Permissive mode để lệnh này hoạt động.
 *
 * Trả về: exit code của lệnh, hoặc giá trị âm nếu có lỗi.
 */
int run_cmd(const char *cmd);

/**
 * read_tmpfile() - Đọc nội dung một file tạm vào buffer kernel
 * @path: Đường dẫn đến file cần đọc
 * @buf:  Buffer đích
 * @size: Kích thước buffer (bao gồm byte null)
 *
 * Mở file, đọc toàn bộ nội dung, đóng file.
 * Luôn null-terminate buffer đầu ra.
 *
 * Trả về: Số byte đọc được, hoặc 0 nếu thất bại.
 */
int read_tmpfile(const char *path, char *buf, int size);

/**
 * sanitize() - Loại bỏ các ký tự nguy hiểm trong shell command injection
 * @s:   Chuỗi cần làm sạch (sửa trực tiếp in-place)
 * @len: Độ dài tối đa cần kiểm tra
 *
 * Thay thế các ký tự đặc biệt của shell (', ", `, $, ;, |, &, \, (), <>)
 * bằng dấu gạch dưới '_'. Dùng trước khi nhúng SSID/password vào lệnh shell.
 */
void sanitize(char *s, int len);

/**
 * set_resp() - Ghi chuỗi vào resp_buf và đánh thức read() đang chờ
 * @mon:  Con trỏ tới trạng thái module
 * @text: Chuỗi phản hồi cần ghi
 *
 * Hàm này thread-safe, tự giữ cmd_lock và gọi wake_up_interruptible().
 */
void set_resp(struct rtl8188_mon *mon, const char *text);

/* ================================================================
 * Khai báo extern các hàm từ các file con
 * ================================================================ */

/* rtl8188_usb.c */
void rtl8188_usb_init(void);
void rtl8188_usb_exit(void);

/* rtl8188_netdev.c */
void rtl8188_netdev_init(void);
void rtl8188_netdev_exit(void);
void scan_existing_netdev(void);

/* rtl8188_pkt.c */
int rtl8188_pkt_recv(struct sk_buff *skb, struct net_device *dev,
		     struct packet_type *pt, struct net_device *orig_dev);

/* rtl8188_cmd.c */
void generate_info(struct rtl8188_mon *mon);
void generate_status(struct rtl8188_mon *mon);
void generate_stats(struct rtl8188_mon *mon);
void generate_capture(struct rtl8188_mon *mon);
void scan_work_fn(struct work_struct *work);
void connect_work_fn(struct work_struct *work);
void disconnect_work_fn(struct work_struct *work);

/* rtl8188_chrdev.c */
int  rtl8188_chrdev_init(void);
void rtl8188_chrdev_exit(void);

/* rtl8188_proc.c */
int  rtl8188_proc_init(void);
void rtl8188_proc_exit(void);

#endif /* RTL8188_MON_H */