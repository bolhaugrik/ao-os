/* AOP-kliens: keretezes, PSK-kezfogas es titkositas (docs/AOP.md). */
#include "aop.h"
#include "crypto.h"
#include "../kernel/lib/string.h"

#define MAGIC "AOP1"
#define FLAG_ENC 1

u8 aop_payload[AOP_PAYLOAD_MAX + 64];
static u8 sealbuf[AOP_PAYLOAD_MAX + 64];
static int sock = -1;
static bool enc;
static struct aochan chan;
static u8 psk[32];
static char model[64];

static int hexval(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }

static bool parse_hex(const char *s, u8 *out, usize n)
{
    for (usize i = 0; i < n; i++) {
        int a = hexval(s[2 * i]), b = hexval(s[2 * i + 1]);
        if (a < 0 || b < 0) return false;
        out[i] = (u8)(a * 16 + b);
    }
    return true;
}

static void to_hex(const u8 *in, usize n, char *out)
{
    static const char d[] = "0123456789abcdef";
    for (usize i = 0; i < n; i++) { out[2 * i] = d[in[i] >> 4]; out[2 * i + 1] = d[in[i] & 15]; }
    out[2 * n] = 0;
}

/* 8 veletlen bajt: TSC + tick + pid keverve ChaCha20-szal. A nonce egyediseget a hid
 * sajat, jo minosegu veletlenje is biztositja; a biztonsag a PSK-n nyugszik. */
static void rand8(u8 out[8])
{
    u32 lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    u8 nonce[12], block[64];
    u64 t = ao_ticks(), p = (u64)ao_getpid();
    nonce[0] = lo & 0xFF; nonce[1] = (lo >> 8) & 0xFF; nonce[2] = (lo >> 16) & 0xFF; nonce[3] = lo >> 24;
    nonce[4] = hi & 0xFF; nonce[5] = (hi >> 8) & 0xFF; nonce[6] = t & 0xFF; nonce[7] = (t >> 8) & 0xFF;
    nonce[8] = (t >> 16) & 0xFF; nonce[9] = (t >> 24) & 0xFF; nonce[10] = p & 0xFF; nonce[11] = (p >> 8) & 0xFF;
    chacha20_block(psk, (u32)(t ^ lo), nonce, block);
    memcpy(out, block + 16, 8);
}

static void put_u16(u8 *p, u16 v) { p[0] = v & 0xFF; p[1] = v >> 8; }
static void put_u32(u8 *p, u32 v) { p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; p[2] = (v >> 16) & 0xFF; p[3] = v >> 24; }
static u16 get_u16(const u8 *p) { return (u16)(p[0] | (p[1] << 8)); }
static u32 get_u32(const u8 *p) { return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24); }

static int send_all(const void *buf, usize n)
{
    const u8 *p = buf;
    usize done = 0;
    while (done < n) {
        isize r = ao_write(sock, p + done, n - done);
        if (r <= 0) return (int)(r ? r : E_PIPE);
        done += (usize)r;
    }
    return 0;
}

static int recv_all(void *buf, usize n)
{
    u8 *p = buf;
    usize done = 0;
    while (done < n) {
        isize r = ao_read(sock, p + done, n - done);
        if (r <= 0) return (int)(r ? r : E_PIPE);
        done += (usize)r;
    }
    return 0;
}

int aop_send(u16 type, const void *data, usize len)
{
    u8 hdr[12];
    memcpy(hdr, MAGIC, 4);
    put_u16(hdr + 4, type);
    put_u16(hdr + 6, enc ? FLAG_ENC : 0);
    put_u32(hdr + 8, (u32)(enc ? len + 16 : len));
    int e = send_all(hdr, 12);
    if (e) return e;
    if (!enc) return len ? send_all(data, len) : 0;
    if (len + 16 > sizeof sealbuf) return E_LIMIT;
    aochan_seal(&chan, hdr, data, len, sealbuf);       /* AAD = a fejlec */
    return send_all(sealbuf, len + 16);
}

static int recv_frame(u16 *type, usize *len)
{
    u8 hdr[12];
    int e = recv_all(hdr, 12);
    if (e) return e;
    if (memcmp(hdr, MAGIC, 4) != 0) return E_INVAL;
    *type = get_u16(hdr + 4);
    u16 flags = get_u16(hdr + 6);
    u32 l = get_u32(hdr + 8);
    if (l >= sizeof aop_payload - 16) return E_LIMIT;
    e = recv_all(aop_payload, l);
    if (e) return e;
    if (flags & FLAG_ENC) {
        if (!enc) return E_INVAL;                           /* titkositott keret kezfogas nelkul */
        if (!aochan_open(&chan, hdr, aop_payload, l, aop_payload)) return E_INVAL;   /* rossz tag/szamlalo */
        l -= 16;
    } else if (enc && *type != AOP_ERR) {
        return E_INVAL;                                     /* titkositatlan keret a csatornan */
    }
    aop_payload[l] = 0;
    *len = l;
    return 0;
}

int aop_recv(u16 *type, usize *len)
{
    for (;;) {
        int e = recv_frame(type, len);
        if (e) return e;
        if (*type == AOP_PING) { aop_send(AOP_PONG, NULL, 0); continue; }
        return 0;
    }
}

static usize read_small(const char *path, char *buf, usize cap)
{
    int fd = ao_open(path, O_READ);
    if (fd < 0) return 0;
    usize got = 0;
    for (;;) {
        isize r = ao_read(fd, buf + got, cap - 1 - got);
        if (r <= 0) break;
        got += (usize)r;
        if (got >= cap - 1) break;
    }
    ao_close(fd);
    buf[got] = 0;
    return got;
}

int aop_connect(const char *agent_name, bool quiet)
{
    static char addr[64], pskhex[80];
    enc = false;
    model[0] = 0;
    if (!read_small("/state/ai/bridge", addr, sizeof addr) && !read_small("/etc/ai/bridge", addr, sizeof addr)) {
        ao_puts("aop: nincs hid-cim (/state/ai/bridge: ip:port)\n");
        return 2;
    }
    for (usize i = 0; addr[i]; i++) if (addr[i] == '\n' || addr[i] == '\r') { addr[i] = 0; break; }

    /* PSK: /state/ai/psk (64 hex). Ha van, a kezfogas utan minden keret titkositott. */
    bool have_psk = read_small("/state/ai/psk", pskhex, sizeof pskhex) >= 64 && parse_hex(pskhex, psk, 32);

    if (!quiet) ao_printf("[agent %s -> hid %s%s]\n", agent_name, addr, have_psk ? ", PSK" : "");
    sock = ao_net_connect(addr);
    if (sock < 0) { ao_printf("aop: kapcsolodas: %s\n", ao_errstr(sock)); return 3; }

    u8 cnonce[8];
    char hello[128];
    usize hl = 0;
    memcpy(hello, "agent=", 6); hl = 6;
    usize nl = strlen(agent_name);
    if (nl > 40) nl = 40;
    memcpy(hello + hl, agent_name, nl); hl += nl;
    memcpy(hello + hl, "\nversion=1\n", 11); hl += 11;
    if (have_psk) {
        rand8(cnonce);
        memcpy(hello + hl, "nonce=", 6); hl += 6;
        to_hex(cnonce, 8, hello + hl); hl += 16;
        hello[hl++] = '\n';
    }
    aop_send(AOP_HELLO, hello, hl);

    /* kezfogas: HELLO_OK (vagy ERR) elott nem kuldunk tobbet */
    u16 type;
    usize len;
    int e = recv_frame(&type, &len);
    if (e) { ao_printf("aop: kezfogas: %s\n", ao_errstr(e)); aop_close(); return 4; }
    if (type == AOP_ERR) { ao_printf("[hid hiba: %s]\n", (char *)aop_payload); aop_close(); return 5; }
    if (type != AOP_HELLO_OK) { ao_puts("aop: varatlan valasz a kezfogasban\n"); aop_close(); return 4; }
    const char *p = (const char *)aop_payload;
    const char *np = NULL;
    bool server_enc = false;
    for (usize i = 0; i + 6 <= len; i++) {
        if ((i == 0 || p[i - 1] == '\n') && memcmp(p + i, "nonce=", 6) == 0) np = p + i + 6;
        if ((i == 0 || p[i - 1] == '\n') && memcmp(p + i, "enc=1", 5) == 0) server_enc = true;
    }
    for (usize i = 0; i < len; i++) if (aop_payload[i] == '\n') { aop_payload[i] = 0; break; }
    if (memcmp(p, "model=", 6) == 0) strlcpy(model, p + 6, sizeof model);
    if (have_psk) {
        u8 snonce[8];
        if (!server_enc || !np || !parse_hex(np, snonce, 8)) {
            ao_puts("aop: a netbookon van PSK, de a hid nem titkosit (inditsd --psk-file kapcsoloval)\n");
            aop_close();
            return 6;
        }
        aochan_init(&chan, psk, cnonce, snonce, false);
        enc = true;
        if (!quiet) ao_printf("[hid: %s, titkositott csatorna]\n", p);
    } else if (!quiet) {
        ao_printf("[hid: %s]\n", p);
    }
    return 0;
}

void aop_close(void)
{
    if (sock >= 0) ao_close(sock);
    sock = -1;
    enc = false;
}

bool aop_encrypted(void) { return enc; }
const char *aop_model(void) { return model; }
