/* Minimalis IPv4-stack: Ethernet, ARP, IPv4, ICMP echo, UDP, TCP (kulon), DHCP (kulon).
 * Egy halozati eszkoz, polling + IRQ, nincs fragmentacio, nincs IPv6, nincs DNS. */
#pragma once
#include "types.h"

#define ETH_MTU 1500
#define FRAME_MAX 1536

struct netdev {
    char name[8];
    u8 mac[6];
    bool up;
    int  (*send)(struct netdev *d, const void *frame, usize len);
    void (*poll)(struct netdev *d);      /* RX-gyuru urites (IRQ-bol es tickbol) */
    u64 rx_packets, tx_packets, rx_bytes, tx_bytes, rx_dropped;
};

static inline u16 htons(u16 v) { return (u16)((v << 8) | (v >> 8)); }
static inline u16 ntohs(u16 v) { return htons(v); }
static inline u32 htonl(u32 v) { return ((v & 0xFF) << 24) | ((v & 0xFF00) << 8) | ((v >> 8) & 0xFF00) | (v >> 24); }
static inline u32 ntohl(u32 v) { return htonl(v); }

/* IP-cimek host-sorrendben (u32), pl. 10.0.2.15 = 0x0A00020F */
struct net_config {
    u32 ip, mask, gw, dns;
    bool configured;
};
extern struct net_config net_cfg;

void net_init(void);
void net_register(struct netdev *d);
struct netdev *net_dev(void);
void net_rx(struct netdev *d, const u8 *frame, usize len);   /* driver hivja */
void net_poll(void);                                          /* PIT tick + IRQ utan */

/* kuldes */
int  net_send_ip(u32 dst, u8 proto, const void *payload, usize len);
int  net_send_udp(u32 dst, u16 sport, u16 dport, const void *payload, usize len);
bool arp_lookup(u32 ip, u8 mac[6]);
void arp_request(u32 ip);
int  net_resolve(u32 ip, u8 mac[6], u32 timeout_ms);         /* ARP blokkolo feloldas */
u32  net_next_hop(u32 dst);

/* UDP fogado: port -> callback */
typedef void (*udp_handler_fn)(u32 src_ip, u16 sport, const u8 *data, usize len);
int  udp_bind(u16 port, udp_handler_fn fn);
void udp_unbind(u16 port);

/* ICMP echo */
int  net_ping(u32 dst, u32 timeout_ms, u32 *rtt_us);

/* segedek */
bool ip_parse(const char *s, u32 *ip);
void ip_format(u32 ip, char *buf, usize cap);
u16  ip_checksum(const void *data, usize len);
u16  net_ephemeral_port(void);
void net_stats(u64 *rx, u64 *tx, u64 *drop);
