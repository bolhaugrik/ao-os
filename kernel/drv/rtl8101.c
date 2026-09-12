/* Realtek RTL8101E-csalad (PCI 10ec:8136) driver, a netbook beepitett Fast Ethernetje.
 * Az RTL8169-fele leiro-gyurus modell: 16 bajtos RX/TX leirok, OWN/EOR/FS/LS bitek.
 * I/O-portos regiszter-eleres (BAR0), polling + IRQ. Nincs offload, nincs VLAN, nincs WoL. */
#include "rtl8101.h"
#include "pci.h"
#include "layout.h"
#include "syscall.h"
#include "../arch/io.h"
#include "../cpu/idt.h"
#include "../cpu/pic.h"
#include "../cpu/pit.h"
#include "../cpu/tsc.h"
#include "../mm/pmm.h"
#include "../net/net.h"
#include "../lib/string.h"

/* regiszterek (bajt-eltolas az I/O bazistol) */
#define R_MAC0      0x00
#define R_MAR0      0x08
#define R_TXDESC    0x20    /* TxDescStartAddr (64 bit) */
#define R_CMD       0x37
#define R_TXPOLL    0x38
#define R_IMR       0x3C
#define R_ISR       0x3E
#define R_TXCFG     0x40
#define R_RXCFG     0x44
#define R_CFG9346   0x50
#define R_CONFIG1   0x52
#define R_PHYSTATUS 0x6C
#define R_RXMAXSIZE 0xDA
#define R_CPLUSCMD  0xE0
#define R_RXDESC    0xE4    /* RxDescAddr (64 bit) */
#define R_ETTHR     0xEC

#define CMD_RESET   0x10
#define CMD_RXEN    0x08
#define CMD_TXEN    0x04

#define DESC_OWN    (1u << 31)
#define DESC_EOR    (1u << 30)
#define DESC_FS     (1u << 29)
#define DESC_LS     (1u << 28)

#define NRX 32
#define NTX 32
#define BUFSZ 2048

struct desc { u32 opts1; u32 opts2; u64 addr; } PACKED;

static u16 io;
static struct desc *rxd, *txd;
static u64 rxd_pa, txd_pa, rxbuf_pa, txbuf_pa;
static u8 *rxbuf, *txbuf;
static u32 rx_cur, tx_cur;
static struct netdev dev;

static inline u8  r8(u16 r)  { return inb(io + r); }
static inline u16 r16(u16 r) { return inw(io + r); }
static inline void w8(u16 r, u8 v)   { outb(io + r, v); }
static inline void w16(u16 r, u16 v) { outw(io + r, v); }
static inline void w32(u16 r, u32 v) { outl(io + r, v); }

static void rtl_poll(struct netdev *d)
{
    for (int n = 0; n < NRX; n++) {
        struct desc *rd = &rxd[rx_cur];
        u32 o = rd->opts1;
        if (o & DESC_OWN) break;
        u32 len = o & 0x3FFF;
        /* a leiroban a hossz tartalmazza a CRC-t (4 bajt) */
        if ((o & (DESC_FS | DESC_LS)) == (DESC_FS | DESC_LS) && len > 18 && !(o & (1u << 21)))  /* RES */
            net_rx(d, rxbuf + (usize)rx_cur * BUFSZ, len - 4);
        else
            d->rx_dropped++;
        rd->opts1 = DESC_OWN | (rx_cur == NRX - 1 ? DESC_EOR : 0) | BUFSZ;
        rx_cur = (rx_cur + 1) % NRX;
    }
}

static int rtl_send(struct netdev *d, const void *frame, usize len)
{
    (void)d;
    if (len > BUFSZ) return E_INVAL;
    struct desc *td = &txd[tx_cur];
    u64 end = rdtsc() + tsc_hz() / 10;
    while (td->opts1 & DESC_OWN) if (rdtsc() > end) return E_TIMEOUT;
    memcpy(txbuf + (usize)tx_cur * BUFSZ, frame, len);
    td->addr = txbuf_pa + (u64)tx_cur * BUFSZ;
    td->opts2 = 0;
    td->opts1 = DESC_OWN | DESC_FS | DESC_LS | (tx_cur == NTX - 1 ? DESC_EOR : 0) | (u32)len;
    tx_cur = (tx_cur + 1) % NTX;
    w8(R_TXPOLL, 0x40);                     /* NPQ: normal prioritasu sor */
    return 0;
}

static void rtl_irq(struct regs *r)
{
    (void)r;
    u16 isr = r16(R_ISR);
    w16(R_ISR, isr);
    if (isr) rtl_poll(&dev);
}

bool rtl8101_init(void)
{
    const struct pci_dev *d = NULL;
    for (u32 i = 0; i < pci_count(); i++) {
        const struct pci_dev *p = pci_get(i);
        if (p->vendor == 0x10EC && (p->device == 0x8136 || p->device == 0x8168 || p->device == 0x8169 || p->device == 0x8167)) { d = p; break; }
    }
    if (!d) return false;
    u32 bar0 = d->bar[0];
    if (!(bar0 & 1)) return false;          /* I/O BAR-t varunk */
    io = (u16)(bar0 & ~3u);
    u32 cmd = pci_read32(d->bus, d->dev, d->fn, 4);
    pci_write32(d->bus, d->dev, d->fn, 4, (cmd | 0x5) & ~0x400u);   /* IO + bus master */

    /* reset */
    w8(R_CMD, CMD_RESET);
    u64 end = rdtsc() + tsc_hz() / 2;
    while ((r8(R_CMD) & CMD_RESET) && rdtsc() < end) ;
    if (r8(R_CMD) & CMD_RESET) return false;

    for (int i = 0; i < 6; i++) dev.mac[i] = r8(R_MAC0 + i);

    rxd_pa = pmm_alloc(); txd_pa = pmm_alloc();
    rxbuf_pa = pmm_alloc_contig(NRX * BUFSZ / PAGE_SIZE);
    txbuf_pa = pmm_alloc_contig(NTX * BUFSZ / PAGE_SIZE);
    if (!rxd_pa || !txd_pa || !rxbuf_pa || !txbuf_pa) return false;
    rxd = P2V(rxd_pa); txd = P2V(txd_pa);
    rxbuf = P2V(rxbuf_pa); txbuf = P2V(txbuf_pa);
    memset(rxd, 0, PAGE_SIZE);
    memset(txd, 0, PAGE_SIZE);
    for (int i = 0; i < NRX; i++) {
        rxd[i].addr = rxbuf_pa + (u64)i * BUFSZ;
        rxd[i].opts1 = DESC_OWN | (i == NRX - 1 ? DESC_EOR : 0) | BUFSZ;
    }
    txd[NTX - 1].opts1 = DESC_EOR;
    rx_cur = tx_cur = 0;

    w8(R_CFG9346, 0xC0);                    /* konfiguracio irhato */
    w16(R_CPLUSCMD, r16(R_CPLUSCMD) | 0x0008 | 0x0001);   /* PCI multiple RW, RX checksum nem kell */
    w16(R_RXMAXSIZE, BUFSZ);
    w32(R_TXDESC, (u32)txd_pa); w32(R_TXDESC + 4, (u32)(txd_pa >> 32));
    w32(R_RXDESC, (u32)rxd_pa); w32(R_RXDESC + 4, (u32)(rxd_pa >> 32));
    w8(R_CMD, CMD_RXEN | CMD_TXEN);
    /* TxConfig: IFG normal, DMA burst unlimited; RxConfig: accept broadcast + phys match, DMA burst unlimited */
    w32(R_TXCFG, 0x03000700);
    w32(R_RXCFG, (7 << 13) | (7 << 8) | 0x0A);
    w8(R_ETTHR, 0x3F);
    w32(R_MAR0, 0xFFFFFFFF); w32(R_MAR0 + 4, 0xFFFFFFFF);
    w8(R_CFG9346, 0x00);

    w16(R_ISR, 0xFFFF);
    strcpy(dev.name, "rtl8101");
    dev.send = rtl_send;
    dev.poll = rtl_poll;
    dev.up = true;
    net_register(&dev);

    if (d->irq_line && d->irq_line < 16) {
        irq_register(d->irq_line, rtl_irq);
        pic_unmask(d->irq_line);
        w16(R_IMR, 0x0001 | 0x0004 | 0x0010 | 0x0020);   /* ROK | TOK | RxOverflow | LinkChg */
    }
    return true;
}
