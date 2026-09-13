#include "tcp.h"
#include "net.h"
#include "syscall.h"
#include "../arch/io.h"
#include "../cpu/pit.h"
#include "../cpu/tsc.h"
#include "../task/task.h"
#include "../mm/kheap.h"
#include "../lib/string.h"

/* Kuldes: csuszo ablak (TCP_TXBUF bajt uton lehet, a partner ablakan belul), go-back-N
 * ujrakuldes RTO-nal. Fogadas: sorrendben erkezo adat gyurube, ablak-frissites kiolvasaskor.
 * A stop-and-wait kuldes a Windows kesleltetett ACK-ja miatt (200 ms/szegmens) ~7 KB/s volt. */

enum { T_CLOSED = 0, T_SYN_SENT, T_ESTABLISHED, T_FIN_WAIT, T_CLOSE_WAIT, T_LAST_ACK, T_TIME_WAIT };
#define F_FIN 1
#define F_SYN 2
#define F_RST 4
#define F_PSH 8
#define F_ACK 16

struct tcp_hdr {
    u16 sport, dport;
    u32 seq, ack;
    u8  off, flags;
    u16 win, csum, urg;
} PACKED;

struct sock {
    int state;
    u32 rip;
    u16 rport, lport;
    u32 snd_nxt, snd_una, rcv_nxt;
    u32 snd_wnd;                /* a partner hirdetett ablaka */
    u8 *rx;                     /* fogado gyuru */
    u32 rx_head, rx_tail, rx_count;
    u8 *tx;                     /* kuldo gyuru: a snd_una-tol meg nem nyugtazott + meg nem kuldott bajtok */
    u32 tx_head, tx_len;        /* tx_head: a snd_una bajt indexe; tx_len: bajtok snd_una-tol */
    u8  ctl_flags;              /* uton levo SYN/FIN (adat nelkul) */
    u32 ctl_seq;
    u64 rto_tick;
    int retries;
    bool peer_closed;
    bool reset;
    struct waitq q;
    u64 last_activity;
    u32 adv_win;                /* az utoljara hirdetett fogadoablak */
};

static struct sock socks[TCP_SOCKS];

void tcp_init(void)
{
    memset(socks, 0, sizeof socks);
}

static const char *names[] = { "closed", "syn-sent", "established", "fin-wait", "close-wait", "last-ack", "time-wait" };
const char *tcp_state_name(int s) { return (s >= 0 && s < TCP_SOCKS) ? names[socks[s].state] : "?"; }
bool tcp_connected(int s) { return s >= 0 && s < TCP_SOCKS && socks[s].state == T_ESTABLISHED; }
usize tcp_available(int s) { return (s >= 0 && s < TCP_SOCKS) ? socks[s].rx_count : 0; }

static u16 tcp_csum(u32 src, u32 dst, const void *seg, usize len)
{
    u8 tmp[FRAME_MAX];
    u32 s = htonl(src), d = htonl(dst);
    memcpy(tmp, &s, 4);
    memcpy(tmp + 4, &d, 4);
    tmp[8] = 0; tmp[9] = 6;
    u16 l = htons((u16)len);
    memcpy(tmp + 10, &l, 2);
    memcpy(tmp + 12, seg, len);
    return ip_checksum(tmp, 12 + len);
}

static int send_seg(struct sock *s, u32 seq, u8 flags, const void *data, usize len)
{
    u8 pkt[FRAME_MAX];
    struct tcp_hdr *h = (struct tcp_hdr *)pkt;
    memset(h, 0, sizeof *h);
    h->sport = htons(s->lport);
    h->dport = htons(s->rport);
    h->seq = htonl(seq);
    h->ack = htonl(s->rcv_nxt);
    h->off = (u8)((sizeof *h / 4) << 4);
    h->flags = flags;
    u32 win = TCP_RXBUF - s->rx_count;
    if (win > 65535) win = 65535;
    s->adv_win = win;
    h->win = htons((u16)win);
    if (len) memcpy(pkt + sizeof *h, data, len);
    h->csum = htons(tcp_csum(net_cfg.ip, s->rip, pkt, sizeof *h + len));
    return net_send_ip(s->rip, 6, pkt, sizeof *h + len);
}

static inline u32 inflight(struct sock *s) { return s->snd_nxt - s->snd_una; }

/* a kuldo gyuru [off, off+n) darabja a snd_una-tol szamitva -> linearis pufferbe */
static void tx_gather(struct sock *s, u32 off, u8 *out, u32 n)
{
    u32 i = (s->tx_head + off) % TCP_TXBUF;
    for (u32 k = 0; k < n; k++) { out[k] = s->tx[i]; i = (i + 1) % TCP_TXBUF; }
}

static void arm_rto(struct sock *s)
{
    s->rto_tick = pit_ticks() + 50;         /* 500 ms */
}

/* uj szegmensek kuldese, amig van adat es fer az ablakba (IF=0 mellett) */
static void tx_pump(struct sock *s)
{
    u32 wnd = s->snd_wnd ? s->snd_wnd : TCP_MSS;   /* zero window: egy szegmensnyi proba */
    if (wnd > TCP_TXBUF) wnd = TCP_TXBUF;
    while (inflight(s) < s->tx_len && inflight(s) < wnd) {
        u32 off = inflight(s);
        u32 n = s->tx_len - off;
        if (n > TCP_MSS) n = TCP_MSS;
        if (n > wnd - inflight(s)) n = wnd - inflight(s);
        u8 buf[TCP_MSS];
        tx_gather(s, off, buf, n);
        bool first = inflight(s) == 0;
        if (send_seg(s, s->snd_nxt, F_ACK | F_PSH, buf, n)) break;
        s->snd_nxt += n;
        if (first) { s->retries = 0; arm_rto(s); }
    }
}

/* vezerlo szegmens (SYN/FIN) kuldese es nyilvantartasa */
static int transmit_ctl(struct sock *s, u8 flags)
{
    s->ctl_seq = s->snd_nxt;
    s->ctl_flags = flags;
    s->retries = 0;
    arm_rto(s);
    s->snd_nxt += 1;
    return send_seg(s, s->ctl_seq, flags, NULL, 0);
}

void tcp_tick(void)
{
    u64 now = pit_ticks();
    for (int i = 0; i < TCP_SOCKS; i++) {
        struct sock *s = &socks[i];
        if (s->state == T_CLOSED) continue;
        if (s->state == T_TIME_WAIT) {
            if (now > s->last_activity + 200) { s->state = T_CLOSED; waitq_wake_all(&s->q); }
            continue;
        }
        if (!inflight(s) && s->tx_len && (s->state == T_ESTABLISHED || s->state == T_CLOSE_WAIT))
            tx_pump(s);                 /* el nem kuldott adat (pl. atmeneti kuldesi hiba utan): ujra probaljuk */
        if (inflight(s) && now >= s->rto_tick) {
            if (++s->retries > 8) {
                s->reset = true;
                s->state = T_CLOSED;
                waitq_wake_all(&s->q);
                continue;
            }
            s->rto_tick = now + 50 * (u64)(1 << (s->retries > 4 ? 4 : s->retries));
            if (s->ctl_flags) {
                send_seg(s, s->ctl_seq, s->ctl_flags, NULL, 0);
            } else {
                /* go-back-N: az elso nyugtazatlan szegmens ujra; a tobbi az ACK utan megy */
                u32 n = inflight(s) < TCP_MSS ? inflight(s) : TCP_MSS;
                u8 buf[TCP_MSS];
                tx_gather(s, 0, buf, n);
                send_seg(s, s->snd_una, F_ACK | F_PSH, buf, n);
            }
        }
    }
}

static void rx_push(struct sock *s, const u8 *d, usize n)
{
    for (usize i = 0; i < n; i++) {
        s->rx[s->rx_head] = d[i];
        s->rx_head = (s->rx_head + 1) % TCP_RXBUF;
        s->rx_count++;
    }
}

void tcp_rx(u32 src, u32 dst, const u8 *seg, usize len)
{
    (void)dst;
    if (len < sizeof(struct tcp_hdr)) return;
    const struct tcp_hdr *h = (const struct tcp_hdr *)seg;
    usize hl = (h->off >> 4) * 4;
    if (hl < sizeof *h || hl > len) return;
    u16 lport = ntohs(h->dport), rport = ntohs(h->sport);
    struct sock *s = NULL;
    for (int i = 0; i < TCP_SOCKS; i++)
        if (socks[i].state != T_CLOSED && socks[i].lport == lport && socks[i].rport == rport && socks[i].rip == src) { s = &socks[i]; break; }
    if (!s) return;
    u32 seq = ntohl(h->seq), ack = ntohl(h->ack);
    u8 fl = h->flags;
    const u8 *data = seg + hl;
    usize dl = len - hl;
    s->last_activity = pit_ticks();
    s->snd_wnd = ntohs(h->win);

    if (fl & F_RST) { s->reset = true; s->state = T_CLOSED; waitq_wake_all(&s->q); return; }

    if (s->state == T_SYN_SENT) {
        if ((fl & (F_SYN | F_ACK)) == (F_SYN | F_ACK) && ack == s->snd_nxt) {
            s->rcv_nxt = seq + 1;
            s->snd_una = ack;
            s->ctl_flags = 0;
            s->state = T_ESTABLISHED;
            send_seg(s, s->snd_nxt, F_ACK, NULL, 0);
            waitq_wake_all(&s->q);
        }
        return;
    }

    /* ACK feldolgozasa: kumulativ, a kuldo gyuru elejet szabaditja fel */
    if ((fl & F_ACK) && inflight(s)) {
        i32 d = (i32)(ack - s->snd_una);
        if (d > 0 && (i32)(ack - s->snd_nxt) <= 0) {
            if (s->ctl_flags) {
                /* FIN nyugtazasa: az egyetlen uton levo bajt a vezerlo */
                s->snd_una = ack;
                s->ctl_flags = 0;
                if (s->state == T_FIN_WAIT) s->state = T_TIME_WAIT;
                if (s->state == T_LAST_ACK) s->state = T_CLOSED;
            } else {
                u32 n = (u32)d;
                if (n > s->tx_len) n = s->tx_len;
                s->tx_head = (s->tx_head + n) % TCP_TXBUF;
                s->tx_len -= n;
                s->snd_una = ack;
                if (inflight(s)) { s->retries = 0; arm_rto(s); }
            }
            tx_pump(s);
            waitq_wake_all(&s->q);
        }
    } else if (fl & F_ACK) {
        tx_pump(s);                         /* ablak-frissites: mehet tovabb */
    }

    /* adat: csak sorrendben */
    bool need_ack = false;
    if (dl) {
        if (seq == s->rcv_nxt) {
            usize room = TCP_RXBUF - s->rx_count;
            usize take = dl < room ? dl : room;
            rx_push(s, data, take);
            s->rcv_nxt += (u32)take;
            waitq_wake_all(&s->q);
        }
        need_ack = true;                    /* duplikalt/sorrenden kivuli: az aktualis rcv_nxt-et ACK-oljuk */
    }
    if ((fl & F_FIN) && seq + dl == s->rcv_nxt) {
        s->rcv_nxt++;
        s->peer_closed = true;
        if (s->state == T_ESTABLISHED) s->state = T_CLOSE_WAIT;
        else if (s->state == T_FIN_WAIT) s->state = T_TIME_WAIT;
        need_ack = true;
        waitq_wake_all(&s->q);
    }
    if (need_ack)
        send_seg(s, s->snd_nxt, F_ACK, NULL, 0);
}

/* IF=0 mellett hivando; visszateres utan IF=1 */
static void wait_event(struct sock *s)
{
    if (task_current()) task_block_on(&s->q);
    else idle_enter();
}

int tcp_connect(u32 ip, u16 port, u32 timeout_ms)
{
    int id = -1;
    for (int i = 0; i < TCP_SOCKS; i++)
        if (socks[i].state == T_CLOSED) { id = i; break; }
    if (id < 0) {
        /* nincs szabad hely: a legregebben lezart TIME_WAIT-es ujrahasznalhato (uj helyi port) */
        u64 oldest = ~0ULL;
        for (int i = 0; i < TCP_SOCKS; i++)
            if (socks[i].state == T_TIME_WAIT && socks[i].last_activity < oldest) { oldest = socks[i].last_activity; id = i; }
    }
    if (id < 0) return E_LIMIT;
    struct sock *s = &socks[id];
    if (s->rx) kfree(s->rx);
    if (s->tx) kfree(s->tx);
    memset(s, 0, sizeof *s);
    s->rx = kmalloc(TCP_RXBUF);
    s->tx = kmalloc(TCP_TXBUF);
    if (!s->rx || !s->tx) { if (s->rx) kfree(s->rx); if (s->tx) kfree(s->tx); s->rx = s->tx = NULL; return E_NOMEM; }
    s->rip = ip;
    s->rport = port;
    s->lport = net_ephemeral_port();
    s->snd_nxt = (u32)(rdtsc() & 0xFFFFFF) << 8;
    s->snd_una = s->snd_nxt;
    s->snd_wnd = TCP_MSS;
    s->state = T_SYN_SENT;
    s->last_activity = pit_ticks();
    u8 mac[6];
    int e = net_resolve(net_next_hop(ip), mac, 2000);   /* ARP elore, blokkolva */
    if (e) { s->state = T_CLOSED; return e; }
    e = transmit_ctl(s, F_SYN);
    if (e) { s->state = T_CLOSED; return e; }
    u64 end = pit_ticks() + timeout_ms / 10 + 1;
    while (s->state == T_SYN_SENT && pit_ticks() < end) {
        if (task_current()) {
            cli();
            if (s->state != T_SYN_SENT) { sti(); break; }
            task_block_timeout(&s->q, 200);
        } else idle_enter();
    }
    if (s->state != T_ESTABLISHED) {
        s->state = T_CLOSED;
        return s->reset ? E_PIPE : E_TIMEOUT;
    }
    return id;
}

/* a bajtokat a kuldo gyurube masolja (ha megtelt, var), a kuldes az ablak szerint megy */
isize tcp_send(int id, const void *buf, usize n)
{
    if (id < 0 || id >= TCP_SOCKS) return E_BADF;
    struct sock *s = &socks[id];
    const u8 *p = buf;
    usize done = 0;
    while (done < n) {
        if (s->state != T_ESTABLISHED && s->state != T_CLOSE_WAIT) return done ? (isize)done : E_PIPE;
        cli();
        while (s->tx_len >= TCP_TXBUF && s->state != T_CLOSED) {
            wait_event(s);              /* visszateres utan IF=1 */
            if (task_current() && task_current()->killed) return E_TIMEOUT;
            cli();
        }
        if (s->state == T_CLOSED) { sti(); return done ? (isize)done : E_PIPE; }
        u32 room = TCP_TXBUF - s->tx_len;
        u32 c = n - done < room ? (u32)(n - done) : room;
        u32 i = (s->tx_head + s->tx_len) % TCP_TXBUF;
        for (u32 k = 0; k < c; k++) { s->tx[i] = p[done + k]; i = (i + 1) % TCP_TXBUF; }
        s->tx_len += c;
        done += c;
        tx_pump(s);
        sti();
    }
    return (isize)done;
}

isize tcp_recv(int id, void *buf, usize n, u32 timeout_ms)
{
    if (id < 0 || id >= TCP_SOCKS) return E_BADF;
    struct sock *s = &socks[id];
    u64 end = pit_ticks() + timeout_ms / 10 + 1;
    while (s->rx_count == 0) {
        if (s->peer_closed || s->state == T_CLOSED) return s->reset ? E_PIPE : 0;
        if (timeout_ms && pit_ticks() >= end) return E_TIMEOUT;
        if (task_current()) {
            if (task_current()->killed) return E_TIMEOUT;
            cli();
            if (s->rx_count || s->peer_closed || s->state == T_CLOSED) { sti(); continue; }
            task_block_timeout(&s->q, timeout_ms ? 500 : 1000);
        } else idle_enter();
    }
    u8 *d = buf;
    usize got = 0;
    cli();
    while (got < n && s->rx_count) {
        d[got++] = s->rx[s->rx_tail];
        s->rx_tail = (s->rx_tail + 1) % TCP_RXBUF;
        s->rx_count--;
    }
    /* ablak-frissites: ha a hirdetett ablak kicsi volt, es most felszabadult a hely, szolunk a
     * kuldonek, kulonben csak a persist-probai (masodpercek) utan folytatna */
    if (s->adv_win < TCP_RXBUF / 2 && TCP_RXBUF - s->rx_count >= TCP_RXBUF / 2 &&
        (s->state == T_ESTABLISHED || s->state == T_CLOSE_WAIT))
        send_seg(s, s->snd_nxt, F_ACK, NULL, 0);
    sti();
    return (isize)got;
}

int tcp_close(int id)
{
    if (id < 0 || id >= TCP_SOCKS) return E_BADF;
    struct sock *s = &socks[id];
    if (s->state == T_ESTABLISHED || s->state == T_CLOSE_WAIT) {
        cli();
        u64 end = pit_ticks() + 3000;   /* max 30 s a fuggo adatok nyugtazasara */
        while (inflight(s) && s->state != T_CLOSED && pit_ticks() < end) { task_block_timeout(&s->q, 200); cli(); }
        int next = s->state == T_CLOSE_WAIT ? T_LAST_ACK : T_FIN_WAIT;
        s->state = next;
        transmit_ctl(s, F_FIN | F_ACK);
        sti();
        end = pit_ticks() + 200;
        while (s->state != T_CLOSED && s->state != T_TIME_WAIT && pit_ticks() < end) {
            if (task_current()) task_sleep_ms(10); else idle_enter();
        }
    }
    s->state = s->state == T_TIME_WAIT ? T_TIME_WAIT : T_CLOSED;
    if (s->state == T_CLOSED) {
        if (s->rx) { kfree(s->rx); s->rx = NULL; }
        if (s->tx) { kfree(s->tx); s->tx = NULL; }
    }
    return 0;
}
