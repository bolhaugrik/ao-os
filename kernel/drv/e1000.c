/* Intel e1000 (82540EM) driver a QEMU-hoz. 32 RX + 32 TX leiro, 2 KiB pufferek,
 * IRQ + tick-polling. A valodi HW-n a Realtek RTL8101E driver (rtl8101.c) fut. */
#include "e1000.h"
#include "pci.h"
#include "layout.h"
#include "syscall.h"
#include "../arch/io.h"
#include "../cpu/idt.h"
#include "../cpu/pic.h"
#include "../cpu/tsc.h"
#include "../mm/pmm.h"
#include "../mm/vmm.h"
#include "../net/net.h"
#include "../lib/string.h"

#define R_CTRL   0x0000
#define R_STATUS 0x0008
#define R_EERD   0x0014
#define R_ICR    0x00C0
#define R_IMS    0x00D0
#define R_IMC    0x00D8
#define R_RCTL   0x0100
#define R_TCTL   0x0400
#define R_TIPG   0x0410
#define R_RDBAL  0x2800
#define R_RDBAH  0x2804
#define R_RDLEN  0x2808
#define R_RDH    0x2810
#define R_RDT    0x2818
#define R_TDBAL  0x3800
#define R_TDBAH  0x3804
#define R_TDLEN  0x3808
#define R_TDH    0x3810
#define R_TDT    0x3818
#define R_MTA    0x5200
#define R_RAL    0x5400
#define R_RAH    0x5404

#define NRX 32
#define NTX 32
#define BUFSZ 2048

struct rx_desc { u64 addr; u16 len; u16 csum; u8 status; u8 err; u16 special; } PACKED;
struct tx_desc { u64 addr; u16 len; u8 cso; u8 cmd; u8 status; u8 css; u16 special; } PACKED;

static volatile u8 *mmio;
static struct rx_desc *rxd;
static struct tx_desc *txd;
static u64 rxd_pa, txd_pa, rxbuf_pa, txbuf_pa;
static u8 *rxbuf, *txbuf;
static u32 rx_cur, tx_cur;
static struct netdev dev;

static inline u32 rd(u32 r) { return *(volatile u32 *)(mmio + r); }
static inline void wr(u32 r, u32 v) { *(volatile u32 *)(mmio + r) = v; }

static u16 eeprom_read(u8 addr)
{
    wr(R_EERD, ((u32)addr << 8) | 1);
    for (int i = 0; i < 100000; i++) {
        u32 v = rd(R_EERD);
        if (v & (1 << 4)) return (u16)(v >> 16);
    }
    return 0;
}

static void e1000_poll(struct netdev *d)
{
    while (rxd[rx_cur].status & 1) {
        u16 len = rxd[rx_cur].len;
        if (!(rxd[rx_cur].err) && len >= 14)
            net_rx(d, rxbuf + (usize)rx_cur * BUFSZ, len);
        else
            d->rx_dropped++;
        rxd[rx_cur].status = 0;
        wr(R_RDT, rx_cur);
        rx_cur = (rx_cur + 1) % NRX;
    }
}

static int e1000_send(struct netdev *d, const void *frame, usize len)
{
    (void)d;
    if (len > BUFSZ) return E_INVAL;
    struct tx_desc *t = &txd[tx_cur];
    /* varunk, amig a leiro szabad (DD) vagy meg soha nem hasznaltuk */
    u64 end = rdtsc() + tsc_hz() / 10;
    while (t->cmd && !(t->status & 1)) if (rdtsc() > end) return E_TIMEOUT;
    memcpy(txbuf + (usize)tx_cur * BUFSZ, frame, len);
    t->addr = txbuf_pa + (u64)tx_cur * BUFSZ;
    t->len = (u16)len;
    t->cso = 0;
    t->cmd = (1 << 0) | (1 << 1) | (1 << 3);   /* EOP | IFCS | RS */
    t->status = 0;
    t->css = 0;
    t->special = 0;
    tx_cur = (tx_cur + 1) % NTX;
    wr(R_TDT, tx_cur);
    return 0;
}

static void e1000_irq(struct regs *r)
{
    (void)r;
    u32 icr = rd(R_ICR);
    (void)icr;
    e1000_poll(&dev);
}

bool e1000_init(void)
{
    const struct pci_dev *d = NULL;
    for (u32 i = 0; i < pci_count(); i++) {
        const struct pci_dev *p = pci_get(i);
        if (p->vendor == 0x8086 && (p->device == 0x100E || p->device == 0x100F || p->device == 0x10D3)) { d = p; break; }
    }
    if (!d) return false;
    u64 bar = d->bar[0] & ~0xFULL;
    if (!bar) return false;
    u32 cmd = pci_read32(d->bus, d->dev, d->fn, 4);
    pci_write32(d->bus, d->dev, d->fn, 4, (cmd | 0x7) & ~0x400u);
    vmm_set_uc(bar, 0x20000);
    mmio = P2V(bar);

    /* MAC: EEPROM, ha van, kulonben RAL/RAH */
    u32 ral = rd(R_RAL), rah = rd(R_RAH);
    if (rah & (1u << 31) && ral) {
        dev.mac[0] = ral & 0xFF; dev.mac[1] = (ral >> 8) & 0xFF; dev.mac[2] = (ral >> 16) & 0xFF; dev.mac[3] = ral >> 24;
        dev.mac[4] = rah & 0xFF; dev.mac[5] = (rah >> 8) & 0xFF;
    } else {
        for (int i = 0; i < 3; i++) {
            u16 w = eeprom_read((u8)i);
            dev.mac[i * 2] = w & 0xFF;
            dev.mac[i * 2 + 1] = w >> 8;
        }
        wr(R_RAL, (u32)dev.mac[0] | ((u32)dev.mac[1] << 8) | ((u32)dev.mac[2] << 16) | ((u32)dev.mac[3] << 24));
        wr(R_RAH, (u32)dev.mac[4] | ((u32)dev.mac[5] << 8) | (1u << 31));
    }

    wr(R_CTRL, rd(R_CTRL) | (1 << 6));         /* SLU */
    for (int i = 0; i < 128; i++) wr(R_MTA + i * 4, 0);
    wr(R_IMC, 0xFFFFFFFF);
    rd(R_ICR);

    /* leirok es pufferek */
    rxd_pa = pmm_alloc(); txd_pa = pmm_alloc();
    rxbuf_pa = pmm_alloc_contig(NRX * BUFSZ / PAGE_SIZE);
    txbuf_pa = pmm_alloc_contig(NTX * BUFSZ / PAGE_SIZE);
    if (!rxd_pa || !txd_pa || !rxbuf_pa || !txbuf_pa) return false;
    rxd = P2V(rxd_pa); txd = P2V(txd_pa);
    rxbuf = P2V(rxbuf_pa); txbuf = P2V(txbuf_pa);
    memset(rxd, 0, PAGE_SIZE);
    memset(txd, 0, PAGE_SIZE);
    for (int i = 0; i < NRX; i++) rxd[i].addr = rxbuf_pa + (u64)i * BUFSZ;

    wr(R_RDBAL, (u32)rxd_pa); wr(R_RDBAH, (u32)(rxd_pa >> 32));
    wr(R_RDLEN, NRX * sizeof(struct rx_desc));
    wr(R_RDH, 0); wr(R_RDT, NRX - 1);
    rx_cur = 0;
    /* RCTL: EN | BAM | BSIZE=2048 (BSEX=0, BSIZE=00) | SECRC */
    wr(R_RCTL, (1 << 1) | (1 << 15) | (1 << 26));

    wr(R_TDBAL, (u32)txd_pa); wr(R_TDBAH, (u32)(txd_pa >> 32));
    wr(R_TDLEN, NTX * sizeof(struct tx_desc));
    wr(R_TDH, 0); wr(R_TDT, 0);
    tx_cur = 0;
    wr(R_TCTL, (1 << 1) | (1 << 3) | (15 << 4) | (64 << 12));   /* EN | PSP | CT | COLD */
    wr(R_TIPG, 10 | (8 << 10) | (6 << 20));

    strcpy(dev.name, "e1000");
    dev.send = e1000_send;
    dev.poll = e1000_poll;
    dev.up = true;
    net_register(&dev);

    if (d->irq_line && d->irq_line < 16) {
        irq_register(d->irq_line, e1000_irq);
        pic_unmask(d->irq_line);
        wr(R_IMS, (1 << 7) | (1 << 4) | (1 << 6) | (1 << 0));   /* RXT0 | RXDMT0 | RXO | TXDW */
    }
    return true;
}
