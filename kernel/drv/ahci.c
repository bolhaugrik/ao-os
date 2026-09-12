/* AHCI SATA driver, polling, egy port. Celhardver: AMD Hudson FCH (1022:7800, a BIOS
 * IDE-modban adja at), QEMU: ICH9 AHCI. DMA egyetlen 64 KiB-os bounce-pufferen at. */
#include "ahci.h"
#include "blk.h"
#include "pci.h"
#include "layout.h"
#include "syscall.h"
#include "../arch/io.h"
#include "../cpu/tsc.h"
#include "../mm/pmm.h"
#include "../mm/vmm.h"
#include "../lib/string.h"

/* HBA regiszterek (dword index = bajt-eltolas / 4) */
#define HBA_CAP    (0x00 / 4)
#define HBA_GHC    (0x04 / 4)
#define HBA_IS     (0x08 / 4)
#define HBA_PI     (0x0C / 4)
#define HBA_VS     (0x10 / 4)
#define HBA_CAP2   (0x24 / 4)
#define HBA_BOHC   (0x28 / 4)
#define PORT_BASE  0x100
#define PORT_SIZE  0x80

/* port regiszterek (bajt eltolas) */
#define PX_CLB   0x00
#define PX_CLBU  0x04
#define PX_FB    0x08
#define PX_FBU   0x0C
#define PX_IS    0x10
#define PX_IE    0x14
#define PX_CMD   0x18
#define PX_TFD   0x20
#define PX_SIG   0x24
#define PX_SSTS  0x28
#define PX_SCTL  0x2C
#define PX_SERR  0x30
#define PX_CI    0x38

#define CMD_ST   (1u << 0)
#define CMD_FRE  (1u << 4)
#define CMD_FR   (1u << 14)
#define CMD_CR   (1u << 15)

#define ATA_IDENTIFY       0xEC
#define ATA_READ_DMA_EXT   0x25
#define ATA_WRITE_DMA_EXT  0x35
#define ATA_FLUSH_EXT      0xEA

#define BOUNCE_FRAMES 16                 /* 64 KiB = 128 szektor */
#define MAX_SECTORS   (BOUNCE_FRAMES * PAGE_SIZE / SECTOR_SIZE)

struct cmd_header {
    u16 flags;          /* CFL[4:0], A, W, P, R, B, C, PMP[15:12] */
    u16 prdtl;
    u32 prdbc;
    u32 ctba;
    u32 ctbau;
    u32 res[4];
} PACKED;

struct prdt_entry {
    u32 dba;
    u32 dbau;
    u32 res;
    u32 dbc;            /* bajtszam-1, bit 31 = I */
} PACKED;

struct cmd_table {
    u8 cfis[64];
    u8 acmd[16];
    u8 res[48];
    struct prdt_entry prdt[8];
} PACKED;

static volatile u32 *hba;
static volatile u8 *port;
static u32 port_idx;
static u64 abar;
static u64 area_pa;             /* 4 KiB: CLB @0, FB @0x400, CT @0x800 */
static u64 bounce_pa;
static u8 *bounce;
static bool present;
static u64 nsectors;
static char model[41];
static const char *last_err = "";

const char *ahci_error(void) { return last_err; }

static inline u32 prd(u32 off) { return *(volatile u32 *)(port + off); }
static inline void pwr(u32 off, u32 v) { *(volatile u32 *)(port + off) = v; }

bool amd_sata_to_ahci(struct pci_dev *d)
{
    if (d->vendor != 0x1022 || d->device != 0x7800 || d->subclass != 0x01)
        return false;
    u8 lock = pci_read8(d->bus, d->dev, d->fn, 0x40);
    pci_write8(d->bus, d->dev, d->fn, 0x40, lock | 1);
    pci_write8(d->bus, d->dev, d->fn, 0x09, 0x01);
    pci_write8(d->bus, d->dev, d->fn, 0x0A, 0x06);
    pci_write8(d->bus, d->dev, d->fn, 0x40, lock);
    pci_refresh(d);
    return d->subclass == 0x06;
}

static bool wait_clear(u32 off, u32 mask, u64 ms)
{
    u64 end = rdtsc() + tsc_hz() / 1000 * ms;
    while (prd(off) & mask)
        if (rdtsc() > end) return false;
    return true;
}

static void port_stop(void)
{
    pwr(PX_CMD, prd(PX_CMD) & ~CMD_ST);
    wait_clear(PX_CMD, CMD_CR, 500);
    pwr(PX_CMD, prd(PX_CMD) & ~CMD_FRE);
    wait_clear(PX_CMD, CMD_FR, 500);
}

static void port_start(void)
{
    while (prd(PX_CMD) & CMD_CR) ;
    pwr(PX_CMD, prd(PX_CMD) | CMD_FRE);
    pwr(PX_CMD, prd(PX_CMD) | CMD_ST);
}

/* egy parancs kiadasa es megvarasa; buf = bounce, count szektor */
static int issue(u8 ata_cmd, u64 lba, u32 count, bool write)
{
    struct cmd_header *hdr = P2V(area_pa);
    struct cmd_table *ct = P2V(area_pa + 0x800);
    memset(ct, 0, sizeof *ct);
    u32 bytes = ata_cmd == ATA_IDENTIFY ? 512 : count * SECTOR_SIZE;

    hdr[0].flags = (5 & 0x1F) | (write ? (1 << 6) : 0);   /* CFL=5 dword, W */
    hdr[0].prdtl = bytes ? 1 : 0;
    hdr[0].prdbc = 0;
    hdr[0].ctba = (u32)(area_pa + 0x800);
    hdr[0].ctbau = (u32)((area_pa + 0x800) >> 32);
    if (bytes) {
        ct->prdt[0].dba = (u32)bounce_pa;
        ct->prdt[0].dbau = (u32)(bounce_pa >> 32);
        ct->prdt[0].dbc = bytes - 1;
    }
    u8 *f = ct->cfis;
    f[0] = 0x27;                /* FIS: Register H2D */
    f[1] = 0x80;                /* C: parancs */
    f[2] = ata_cmd;
    f[3] = 0;
    f[4] = lba & 0xFF;
    f[5] = (lba >> 8) & 0xFF;
    f[6] = (lba >> 16) & 0xFF;
    f[7] = 0x40;                /* LBA mod */
    f[8] = (lba >> 24) & 0xFF;
    f[9] = (lba >> 32) & 0xFF;
    f[10] = (lba >> 40) & 0xFF;
    f[11] = 0;
    f[12] = count & 0xFF;
    f[13] = (count >> 8) & 0xFF;

    if (!wait_clear(PX_TFD, 0x88, 1000))     /* BSY | DRQ */
        return E_IO;
    pwr(PX_IS, 0xFFFFFFFF);
    pwr(PX_CI, 1);
    u64 end = rdtsc() + tsc_hz() * 3;
    for (;;) {
        u32 ci = prd(PX_CI), is = prd(PX_IS), tfd = prd(PX_TFD);
        if (is & (1u << 30)) return E_IO;    /* TFES */
        if (tfd & 1) return E_IO;            /* ERR */
        if (!(ci & 1)) break;
        if (rdtsc() > end) return E_TIMEOUT;
    }
    return 0;
}

static void ata_string(char *dst, const u16 *src, int words)
{
    for (int i = 0; i < words; i++) {
        dst[i * 2] = (char)(src[i] >> 8);
        dst[i * 2 + 1] = (char)(src[i] & 0xFF);
    }
    int n = words * 2;
    dst[n] = 0;
    while (n > 0 && dst[n - 1] == ' ') dst[--n] = 0;
}

bool ahci_init(void)
{
    struct pci_dev *dev = NULL;
    for (u32 i = 0; i < pci_count(); i++) {
        struct pci_dev *d = pci_get_mut(i);
        if (d->class_ != 0x01) continue;
        if (d->subclass == 0x01) amd_sata_to_ahci(d);
        if (d->subclass == 0x06) { dev = d; break; }
    }
    if (!dev) { last_err = "nincs AHCI-vezerlo"; return false; }
    abar = dev->bar[5] & ~0xFULL;
    if (!abar || abar >= 4ULL * 1024 * 1024 * 1024) { last_err = "rossz ABAR"; return false; }
    bool found_port = false;

    /* PCI: memoria-ter + bus master */
    u32 cmd = pci_read32(dev->bus, dev->dev, dev->fn, 4);
    pci_write32(dev->bus, dev->dev, dev->fn, 4, (cmd | 0x6) & ~0x400u);

    vmm_set_uc(abar, 0x1100);
    hba = P2V(abar);

    /* BIOS/OS atadas, ha a HBA keri */
    if (hba[HBA_CAP2] & 1) {
        hba[HBA_BOHC] |= 2;                     /* OOS */
        u64 end = rdtsc() + tsc_hz() * 2;
        while ((hba[HBA_BOHC] & 1) && rdtsc() < end) ;
    }
    hba[HBA_GHC] |= (1u << 31);                 /* AHCI enable */

    area_pa = pmm_alloc();
    bounce_pa = pmm_alloc_contig(BOUNCE_FRAMES);
    if (!area_pa || !bounce_pa) { last_err = "nincs DMA-memoria"; return false; }
    memset(P2V(area_pa), 0, PAGE_SIZE);
    bounce = P2V(bounce_pa);

    /* portok, amin eszkoz van (DET=3); a SIG csak a FIS-fogadas utan ervenyes,
     * ezert a port inditasa utan IDENTIFY-jal dontunk */
    u32 pi = hba[HBA_PI];
    bool any = false;
    for (u32 p = 0; p < 32; p++) {
        if (!(pi & (1u << p))) continue;
        port = (volatile u8 *)hba + PORT_BASE + p * PORT_SIZE;
        if ((prd(PX_SSTS) & 0xF) != 3) continue;
        any = true;
        port_idx = p;
        port_stop();
        pwr(PX_CLB, (u32)area_pa);
        pwr(PX_CLBU, (u32)(area_pa >> 32));
        pwr(PX_FB, (u32)(area_pa + 0x400));
        pwr(PX_FBU, (u32)((area_pa + 0x400) >> 32));
        pwr(PX_SERR, 0xFFFFFFFF);
        pwr(PX_IS, 0xFFFFFFFF);
        pwr(PX_IE, 0);
        port_start();
        u64 end = rdtsc() + tsc_hz() / 10;
        while (rdtsc() < end && (prd(PX_TFD) & 0x88)) ;
        u32 sig = prd(PX_SIG);
        if (sig == 0xEB140101) { port_stop(); continue; }   /* ATAPI: kihagyjuk */
        if (issue(ATA_IDENTIFY, 0, 0, false) == 0) { found_port = true; break; }
        port_stop();
    }
    if (!found_port) { last_err = any ? "IDENTIFY sikertelen" : "nincs eszkoz a portokon"; return false; }
    const u16 *id = (const u16 *)bounce;
    ata_string(model, id + 27, 20);
    nsectors = ((u64)id[103] << 48) | ((u64)id[102] << 32) | ((u64)id[101] << 16) | id[100];
    if (!nsectors) nsectors = ((u64)id[61] << 16) | id[60];
    present = true;
    return true;
}

u32 ahci_port_index(void) { return port_idx; }
u64 ahci_abar(void) { return abar; }

/* ---------------------------------------------------------------- blk */
bool blk_present(void) { return present; }
u64 blk_sectors(void) { return nsectors; }
const char *blk_model(void) { return model; }

int blk_read(u64 lba, u32 count, void *buf)
{
    if (!present) return E_IO;
    u8 *d = buf;
    while (count) {
        u32 n = count > MAX_SECTORS ? MAX_SECTORS : count;
        int e = issue(ATA_READ_DMA_EXT, lba, n, false);
        if (e) return e;
        memcpy(d, bounce, n * SECTOR_SIZE);
        d += n * SECTOR_SIZE;
        lba += n;
        count -= n;
    }
    return 0;
}

int blk_write(u64 lba, u32 count, const void *buf)
{
    if (!present) return E_IO;
    const u8 *s = buf;
    while (count) {
        u32 n = count > MAX_SECTORS ? MAX_SECTORS : count;
        memcpy(bounce, s, n * SECTOR_SIZE);
        int e = issue(ATA_WRITE_DMA_EXT, lba, n, true);
        if (e) return e;
        s += n * SECTOR_SIZE;
        lba += n;
        count -= n;
    }
    return 0;
}

int blk_flush(void)
{
    if (!present) return E_IO;
    return issue(ATA_FLUSH_EXT, 0, 0, false);
}
