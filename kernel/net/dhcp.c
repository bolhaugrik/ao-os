#include "dhcp.h"
#include "net.h"
#include "syscall.h"
#include "../arch/io.h"
#include "../cpu/pit.h"
#include "../cpu/tsc.h"
#include "../task/task.h"
#include "../lib/string.h"

struct dhcp_pkt {
    u8 op, htype, hlen, hops;
    u32 xid;
    u16 secs, flags;
    u32 ciaddr, yiaddr, siaddr, giaddr;
    u8 chaddr[16];
    u8 sname[64];
    u8 file[128];
    u32 magic;
    u8 opts[312];
} PACKED;

static u32 xid;
static volatile int stage;          /* 0 var, 1 offer, 2 ack, -1 nak */
static u32 offer_ip, server_ip, offer_mask, offer_gw, offer_dns;

static void parse_opts(const u8 *o, usize len, u8 *msgtype)
{
    usize i = 0;
    while (i + 1 < len) {
        u8 t = o[i], l = o[i + 1];
        if (t == 255) break;
        if (t == 0) { i++; continue; }
        const u8 *v = o + i + 2;
        if (i + 2 + l > len) break;
        switch (t) {
        case 53: *msgtype = v[0]; break;
        case 1:  if (l >= 4) offer_mask = ((u32)v[0] << 24) | (v[1] << 16) | (v[2] << 8) | v[3]; break;
        case 3:  if (l >= 4) offer_gw = ((u32)v[0] << 24) | (v[1] << 16) | (v[2] << 8) | v[3]; break;
        case 6:  if (l >= 4) offer_dns = ((u32)v[0] << 24) | (v[1] << 16) | (v[2] << 8) | v[3]; break;
        case 54: if (l >= 4) server_ip = ((u32)v[0] << 24) | (v[1] << 16) | (v[2] << 8) | v[3]; break;
        }
        i += 2 + l;
    }
}

static void dhcp_rx(u32 src, u16 sport, const u8 *data, usize len)
{
    (void)src; (void)sport;
    if (len < sizeof(struct dhcp_pkt) - 312 + 4) return;
    const struct dhcp_pkt *p = (const struct dhcp_pkt *)data;
    if (p->op != 2 || ntohl(p->xid) != xid || ntohl(p->magic) != 0x63825363) return;
    u8 type = 0;
    parse_opts(p->opts, len - (sizeof *p - 312), &type);
    if (type == 2 && stage == 0) { offer_ip = ntohl(p->yiaddr); stage = 1; }
    else if (type == 5 && stage == 1) { offer_ip = ntohl(p->yiaddr); stage = 2; }
    else if (type == 6) stage = -1;
}

static void send_msg(u8 type)
{
    struct dhcp_pkt p;
    memset(&p, 0, sizeof p);
    p.op = 1; p.htype = 1; p.hlen = 6;
    p.xid = htonl(xid);
    p.flags = htons(0x8000);                    /* broadcast valasz */
    memcpy(p.chaddr, net_dev()->mac, 6);
    p.magic = htonl(0x63825363);
    u8 *o = p.opts;
    *o++ = 53; *o++ = 1; *o++ = type;
    *o++ = 61; *o++ = 7; *o++ = 1; memcpy(o, net_dev()->mac, 6); o += 6;
    if (type == 3) {
        *o++ = 50; *o++ = 4; *o++ = offer_ip >> 24; *o++ = (offer_ip >> 16) & 255; *o++ = (offer_ip >> 8) & 255; *o++ = offer_ip & 255;
        *o++ = 54; *o++ = 4; *o++ = server_ip >> 24; *o++ = (server_ip >> 16) & 255; *o++ = (server_ip >> 8) & 255; *o++ = server_ip & 255;
    }
    *o++ = 55; *o++ = 3; *o++ = 1; *o++ = 3; *o++ = 6;
    *o++ = 12; *o++ = 5; memcpy(o, "ao-os", 5); o += 5;
    *o++ = 255;
    usize len = (usize)(o - (u8 *)&p);
    if (len < 300) len = 300;
    net_send_udp(0xFFFFFFFF, 68, 67, &p, len);
}

int dhcp_run(u32 timeout_ms)
{
    if (!net_dev() || !net_dev()->up) return E_IO;
    xid = (u32)rdtsc() ^ 0xA0A0A0A0u;
    stage = 0;
    offer_ip = server_ip = offer_mask = offer_gw = offer_dns = 0;
    struct net_config saved = net_cfg;
    net_cfg.configured = false;             /* a broadcast forrascime 0.0.0.0 */
    net_cfg.ip = 0;
    udp_bind(68, dhcp_rx);
    u64 end = pit_ticks() + timeout_ms / 10 + 1;
    u64 next = 0;
    int last_stage = -2;
    int rc = E_TIMEOUT;
    while (pit_ticks() < end) {
        if (stage != last_stage || pit_ticks() >= next) {
            if (stage == 0) send_msg(1);
            else if (stage == 1) send_msg(3);
            last_stage = stage;
            next = pit_ticks() + 200;
        }
        if (stage == 2) {
            net_cfg.ip = offer_ip;
            net_cfg.mask = offer_mask ? offer_mask : 0xFFFFFF00;
            net_cfg.gw = offer_gw;
            net_cfg.dns = offer_dns;
            net_cfg.configured = true;
            rc = 0;
            break;
        }
        if (stage == -1) { rc = E_INVAL; break; }
        if (task_current()) task_sleep_ms(10); else idle_enter();
    }
    udp_unbind(68);
    if (rc) net_cfg = saved;
    return rc;
}
