#include "net.h"
#include "tcp.h"
#include "syscall.h"
#include "../arch/io.h"
#include "../cpu/pit.h"
#include "../cpu/tsc.h"
#include "../task/task.h"
#include "../lib/string.h"
#include "../lib/fmt.h"

struct net_config net_cfg;
static struct netdev *dev;

struct eth_hdr { u8 dst[6], src[6]; u16 type; } PACKED;
struct arp_pkt {
    u16 htype, ptype; u8 hlen, plen; u16 op;
    u8 sha[6]; u32 spa; u8 tha[6]; u32 tpa;
} PACKED;
struct ip_hdr {
    u8 vhl, tos; u16 len, id, frag; u8 ttl, proto; u16 csum; u32 src, dst;
} PACKED;
struct icmp_hdr { u8 type, code; u16 csum; u16 id, seq; } PACKED;
struct udp_hdr { u16 sport, dport, len, csum; } PACKED;

#define ETH_ARP 0x0806
#define ETH_IP  0x0800
static const u8 bcast_mac[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

/* ---------------------------------------------------------------- ARP-tabla */
struct arp_ent { u32 ip; u8 mac[6]; bool valid; u64 tick; };
static struct arp_ent arp_tab[16];
static struct waitq arp_q;

void net_init(void)
{
    memset(&net_cfg, 0, sizeof net_cfg);
    memset(arp_tab, 0, sizeof arp_tab);
    tcp_init();
}

void net_register(struct netdev *d) { dev = d; }
struct netdev *net_dev(void) { return dev; }

u16 ip_checksum(const void *data, usize len)
{
    const u8 *p = data;
    u32 sum = 0;
    while (len > 1) { sum += (u32)((p[0] << 8) | p[1]); p += 2; len -= 2; }
    if (len) sum += (u32)(p[0] << 8);
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (u16)~sum;
}

bool ip_parse(const char *s, u32 *ip)
{
    u32 v = 0;
    for (int i = 0; i < 4; i++) {
        u32 n = 0;
        int d = 0;
        while (*s >= '0' && *s <= '9') { n = n * 10 + (u32)(*s++ - '0'); d++; }
        if (!d || n > 255) return false;
        v = (v << 8) | n;
        if (i < 3) { if (*s != '.') return false; s++; }
    }
    if (*s && *s != ':' && *s != ' ') return false;
    *ip = v;
    return true;
}

void ip_format(u32 ip, char *buf, usize cap)
{
    snformat(buf, cap, "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255);
}

static u16 eph_port = 40000;
u16 net_ephemeral_port(void)
{
    if (++eph_port > 60000) eph_port = 40000;
    return eph_port;
}

void net_stats(u64 *rx, u64 *tx, u64 *drop)
{
    *rx = dev ? dev->rx_packets : 0;
    *tx = dev ? dev->tx_packets : 0;
    *drop = dev ? dev->rx_dropped : 0;
}

/* ---------------------------------------------------------------- Ethernet */
static int eth_send(const u8 dst[6], u16 type, const void *payload, usize len)
{
    if (!dev || !dev->up || len > ETH_MTU) return E_IO;
    u8 frame[FRAME_MAX];
    struct eth_hdr *e = (struct eth_hdr *)frame;
    memcpy(e->dst, dst, 6);
    memcpy(e->src, dev->mac, 6);
    e->type = htons(type);
    memcpy(frame + sizeof *e, payload, len);
    usize total = sizeof *e + len;
    if (total < 60) { memset(frame + total, 0, 60 - total); total = 60; }
    int r = dev->send(dev, frame, total);
    if (r == 0) { dev->tx_packets++; dev->tx_bytes += total; }
    return r;
}

/* ---------------------------------------------------------------- ARP */
bool arp_lookup(u32 ip, u8 mac[6])
{
    for (int i = 0; i < 16; i++)
        if (arp_tab[i].valid && arp_tab[i].ip == ip) { memcpy(mac, arp_tab[i].mac, 6); return true; }
    return false;
}

static void arp_insert(u32 ip, const u8 mac[6])
{
    int slot = -1;
    u64 oldest = ~0ULL;
    for (int i = 0; i < 16; i++) {
        if (arp_tab[i].valid && arp_tab[i].ip == ip) { slot = i; break; }
        if (!arp_tab[i].valid) { if (slot < 0) slot = i; }
        else if (arp_tab[i].tick < oldest) { oldest = arp_tab[i].tick; if (slot < 0 || arp_tab[slot].valid) slot = i; }
    }
    if (slot < 0) slot = 0;
    arp_tab[slot].ip = ip;
    memcpy(arp_tab[slot].mac, mac, 6);
    arp_tab[slot].valid = true;
    arp_tab[slot].tick = pit_ticks();
    waitq_wake_all(&arp_q);
}

void arp_request(u32 ip)
{
    struct arp_pkt a;
    a.htype = htons(1); a.ptype = htons(ETH_IP); a.hlen = 6; a.plen = 4; a.op = htons(1);
    memcpy(a.sha, dev->mac, 6);
    a.spa = htonl(net_cfg.ip);
    memset(a.tha, 0, 6);
    a.tpa = htonl(ip);
    eth_send(bcast_mac, ETH_ARP, &a, sizeof a);
}

static void arp_rx(const u8 *p, usize len)
{
    if (len < sizeof(struct arp_pkt)) return;
    const struct arp_pkt *a = (const struct arp_pkt *)p;
    if (ntohs(a->htype) != 1 || ntohs(a->ptype) != ETH_IP) return;
    u32 spa = ntohl(a->spa), tpa = ntohl(a->tpa);
    arp_insert(spa, a->sha);
    if (ntohs(a->op) == 1 && net_cfg.configured && tpa == net_cfg.ip) {
        struct arp_pkt r = *a;
        r.op = htons(2);
        memcpy(r.tha, a->sha, 6);
        r.tpa = a->spa;
        memcpy(r.sha, dev->mac, 6);
        r.spa = htonl(net_cfg.ip);
        eth_send(a->sha, ETH_ARP, &r, sizeof r);
    }
}

u32 net_next_hop(u32 dst)
{
    if (dst == 0xFFFFFFFF) return dst;
    if ((dst & net_cfg.mask) == (net_cfg.ip & net_cfg.mask)) return dst;
    return net_cfg.gw ? net_cfg.gw : dst;
}

int net_resolve(u32 ip, u8 mac[6], u32 timeout_ms)
{
    if (ip == 0xFFFFFFFF) { memcpy(mac, bcast_mac, 6); return 0; }
    if (arp_lookup(ip, mac)) return 0;
    u64 end = pit_ticks() + timeout_ms / 10 + 1;
    u64 next_req = 0;
    while (pit_ticks() < end) {
        if (pit_ticks() >= next_req) { arp_request(ip); next_req = pit_ticks() + 50; }
        if (arp_lookup(ip, mac)) return 0;
        if (task_current()) task_sleep_ms(10); else idle_enter();
    }
    return E_TIMEOUT;
}

/* ---------------------------------------------------------------- IP */
int net_send_ip(u32 dst, u8 proto, const void *payload, usize len)
{
    if (!dev || !dev->up) return E_IO;
    if (!net_cfg.configured && dst != 0xFFFFFFFF) return E_IO;
    if (len + sizeof(struct ip_hdr) > ETH_MTU) return E_INVAL;
    /* nem blokkol: IRQ-kontextusbol is hivhato. Ha nincs ARP-bejegyzes, kerest kuld
     * es E_BUSY-t ad; a connect/ping elore feloldja a kovetkezo ugrast (net_resolve). */
    u8 mac[6];
    u32 hop = net_next_hop(dst);
    if (hop == 0xFFFFFFFF) memcpy(mac, bcast_mac, 6);
    else if (!arp_lookup(hop, mac)) { arp_request(hop); return E_BUSY; }
    static u16 ip_id = 1;
    u8 pkt[FRAME_MAX];
    struct ip_hdr *h = (struct ip_hdr *)pkt;
    h->vhl = 0x45; h->tos = 0;
    h->len = htons((u16)(sizeof *h + len));
    h->id = htons(ip_id++);
    h->frag = htons(0x4000);
    h->ttl = 64; h->proto = proto; h->csum = 0;
    h->src = htonl(net_cfg.ip); h->dst = htonl(dst);
    h->csum = htons(ip_checksum(h, sizeof *h));
    memcpy(pkt + sizeof *h, payload, len);
    return eth_send(mac, ETH_IP, pkt, sizeof *h + len);
}

/* ---------------------------------------------------------------- UDP */
struct udp_bind { u16 port; udp_handler_fn fn; };
static struct udp_bind udp_binds[4];

int udp_bind(u16 port, udp_handler_fn fn)
{
    for (int i = 0; i < 4; i++)
        if (!udp_binds[i].fn) { udp_binds[i].port = port; udp_binds[i].fn = fn; return 0; }
    return E_LIMIT;
}

void udp_unbind(u16 port)
{
    for (int i = 0; i < 4; i++)
        if (udp_binds[i].fn && udp_binds[i].port == port) udp_binds[i].fn = NULL;
}

int net_send_udp(u32 dst, u16 sport, u16 dport, const void *payload, usize len)
{
    u8 pkt[FRAME_MAX];
    struct udp_hdr *u = (struct udp_hdr *)pkt;
    u->sport = htons(sport); u->dport = htons(dport);
    u->len = htons((u16)(sizeof *u + len)); u->csum = 0;   /* UDP checksum opcionalis IPv4-en */
    memcpy(pkt + sizeof *u, payload, len);
    return net_send_ip(dst, 17, pkt, sizeof *u + len);
}

static void udp_rx(u32 src, const u8 *p, usize len)
{
    if (len < sizeof(struct udp_hdr)) return;
    const struct udp_hdr *u = (const struct udp_hdr *)p;
    u16 dport = ntohs(u->dport);
    usize dl = ntohs(u->len);
    if (dl < sizeof *u || dl > len) return;
    for (int i = 0; i < 4; i++)
        if (udp_binds[i].fn && udp_binds[i].port == dport) {
            udp_binds[i].fn(src, ntohs(u->sport), p + sizeof *u, dl - sizeof *u);
            return;
        }
}

/* ---------------------------------------------------------------- ICMP */
static volatile u16 ping_wait_seq;
static volatile bool ping_got;
static struct waitq ping_q;

static void icmp_rx(u32 src, const u8 *p, usize len)
{
    if (len < sizeof(struct icmp_hdr)) return;
    const struct icmp_hdr *h = (const struct icmp_hdr *)p;
    if (h->type == 8) {                     /* echo request -> reply */
        u8 buf[FRAME_MAX];
        if (len > ETH_MTU - 20) return;
        memcpy(buf, p, len);
        struct icmp_hdr *r = (struct icmp_hdr *)buf;
        r->type = 0; r->csum = 0;
        r->csum = htons(ip_checksum(buf, len));
        net_send_ip(src, 1, buf, len);
    } else if (h->type == 0) {
        if (ntohs(h->seq) == ping_wait_seq) { ping_got = true; waitq_wake_all(&ping_q); }
    }
}

int net_ping(u32 dst, u32 timeout_ms, u32 *rtt_us)
{
    static u16 seq = 1;
    u8 pkt[64];
    struct icmp_hdr *h = (struct icmp_hdr *)pkt;
    h->type = 8; h->code = 0; h->csum = 0; h->id = htons(0xA0A0); h->seq = htons(seq);
    for (usize i = sizeof *h; i < sizeof pkt; i++) pkt[i] = (u8)i;
    h->csum = htons(ip_checksum(pkt, sizeof pkt));
    ping_wait_seq = seq++;
    ping_got = false;
    u8 mac[6];
    int e = net_resolve(net_next_hop(dst), mac, timeout_ms);
    if (e) return e;
    u64 t0 = rdtsc();
    e = net_send_ip(dst, 1, pkt, sizeof pkt);
    if (e) return e;
    u64 end = pit_ticks() + timeout_ms / 10 + 1;
    while (!ping_got && pit_ticks() < end) {
        if (task_current()) {
            cli();
            if (ping_got) { sti(); break; }
            task_block_timeout(&ping_q, timeout_ms);
        } else idle_enter();
    }
    if (!ping_got) return E_TIMEOUT;
    *rtt_us = (u32)tsc_to_us(rdtsc() - t0);
    return 0;
}

/* ---------------------------------------------------------------- RX */
void net_rx(struct netdev *d, const u8 *frame, usize len)
{
    d->rx_packets++;
    d->rx_bytes += len;
    if (len < sizeof(struct eth_hdr)) { d->rx_dropped++; return; }
    const struct eth_hdr *e = (const struct eth_hdr *)frame;
    u16 type = ntohs(e->type);
    const u8 *p = frame + sizeof *e;
    usize pl = len - sizeof *e;
    if (type == ETH_ARP) { arp_rx(p, pl); return; }
    if (type != ETH_IP) return;
    if (pl < sizeof(struct ip_hdr)) { d->rx_dropped++; return; }
    const struct ip_hdr *h = (const struct ip_hdr *)p;
    usize hl = (h->vhl & 15) * 4;
    usize tl = ntohs(h->len);
    if ((h->vhl >> 4) != 4 || hl < 20 || tl < hl || tl > pl) { d->rx_dropped++; return; }
    if (ntohs(h->frag) & 0x3FFF) { d->rx_dropped++; return; }      /* fragmens: nem tamogatott */
    u32 dst = ntohl(h->dst), src = ntohl(h->src);
    if (net_cfg.configured && dst != net_cfg.ip && dst != 0xFFFFFFFF && dst != (net_cfg.ip | ~net_cfg.mask)) return;
    /* MAC tanulasa a bejovo csomagbol: a valaszhoz nem kell kulon ARP */
    if (net_cfg.configured && src && net_next_hop(src) == src)
        arp_insert(src, e->src);
    const u8 *body = p + hl;
    usize bl = tl - hl;
    switch (h->proto) {
    case 1:  icmp_rx(src, body, bl); break;
    case 6:  tcp_rx(src, dst, body, bl); break;
    case 17: udp_rx(src, body, bl); break;
    default: break;
    }
}

void net_poll(void)
{
    if (dev && dev->poll) dev->poll(dev);
    tcp_tick();
}
