/* PCI konfiguracios ter a 0xCF8/0xCFC portokon. Csak enumeracio: az AHCI es a NIC
 * megtalalasahoz kell. Nincs eroforras-allokacio, nincs busz-keretrendszer. */
#include "pci.h"
#include "../arch/io.h"

static struct pci_dev devs[PCI_MAX];
static u32 ndevs;

u32 pci_read32(u8 bus, u8 dev, u8 fn, u8 off)
{
    u32 addr = 0x80000000u | ((u32)bus << 16) | ((u32)dev << 11) | ((u32)fn << 8) | (off & 0xFC);
    outl(0xCF8, addr);
    return inl(0xCFC);
}

u8 pci_read8(u8 bus, u8 dev, u8 fn, u8 off)
{
    return (pci_read32(bus, dev, fn, off) >> ((off & 3) * 8)) & 0xFF;
}

void pci_write8(u8 bus, u8 dev, u8 fn, u8 off, u8 v)
{
    u32 addr = 0x80000000u | ((u32)bus << 16) | ((u32)dev << 11) | ((u32)fn << 8) | (off & 0xFC);
    outl(0xCF8, addr);
    outb(0xCFC + (off & 3), v);
}

void pci_write32(u8 bus, u8 dev, u8 fn, u8 off, u32 v)
{
    u32 addr = 0x80000000u | ((u32)bus << 16) | ((u32)dev << 11) | ((u32)fn << 8) | (off & 0xFC);
    outl(0xCF8, addr);
    outl(0xCFC, v);
}

void pci_refresh(struct pci_dev *d)
{
    u8 bus = d->bus, dev = d->dev, fn = d->fn;
    u32 cls = pci_read32(bus, dev, fn, 8);
    d->rev = cls & 0xFF;
    d->progif = (cls >> 8) & 0xFF;
    d->subclass = (cls >> 16) & 0xFF;
    d->class_ = cls >> 24;
    if (d->header == 0)
        for (int i = 0; i < 6; i++)
            d->bar[i] = pci_read32(bus, dev, fn, 0x10 + i * 4);
}

static void probe(u8 bus, u8 dev, u8 fn)
{
    u32 id = pci_read32(bus, dev, fn, 0);
    if ((id & 0xFFFF) == 0xFFFF || ndevs >= PCI_MAX)
        return;
    struct pci_dev *d = &devs[ndevs++];
    d->bus = bus; d->dev = dev; d->fn = fn;
    d->vendor = id & 0xFFFF;
    d->device = id >> 16;
    u32 cls = pci_read32(bus, dev, fn, 8);
    d->rev = cls & 0xFF;
    d->progif = (cls >> 8) & 0xFF;
    d->subclass = (cls >> 16) & 0xFF;
    d->class_ = cls >> 24;
    d->header = (pci_read32(bus, dev, fn, 0x0C) >> 16) & 0x7F;
    if (d->header == 0) {
        for (int i = 0; i < 6; i++)
            d->bar[i] = pci_read32(bus, dev, fn, 0x10 + i * 4);
        d->irq_line = pci_read32(bus, dev, fn, 0x3C) & 0xFF;
    }
}

void pci_init(void)
{
    ndevs = 0;
    for (u32 bus = 0; bus < 8; bus++) {
        for (u8 dev = 0; dev < 32; dev++) {
            u32 id = pci_read32((u8)bus, dev, 0, 0);
            if ((id & 0xFFFF) == 0xFFFF)
                continue;
            u8 hdr = (pci_read32((u8)bus, dev, 0, 0x0C) >> 16) & 0xFF;
            u8 nfn = (hdr & 0x80) ? 8 : 1;
            for (u8 fn = 0; fn < nfn; fn++)
                probe((u8)bus, dev, fn);
        }
    }
}

u32 pci_count(void) { return ndevs; }
const struct pci_dev *pci_get(u32 i) { return i < ndevs ? &devs[i] : NULL; }
struct pci_dev *pci_get_mut(u32 i) { return i < ndevs ? &devs[i] : NULL; }

const char *pci_class_name(u8 c, u8 s)
{
    switch (c) {
    case 0x00: return "regi/ismeretlen";
    case 0x01:
        switch (s) {
        case 0x01: return "IDE";
        case 0x04: return "RAID";
        case 0x06: return "SATA AHCI";
        case 0x08: return "NVMe";
        default:   return "tarolo";
        }
    case 0x02: return s == 0 ? "Ethernet" : (s == 0x80 ? "halozat (egyeb)" : "halozat");
    case 0x03: return "VGA/kijelzo";
    case 0x04: return "multimedia/hang";
    case 0x05: return "memoria";
    case 0x06:
        switch (s) {
        case 0x00: return "host-hid";
        case 0x01: return "ISA/LPC-hid";
        case 0x04: return "PCI-PCI hid";
        default:   return "hid";
        }
    case 0x07: return "soros/parhuzamos";
    case 0x08: return "rendszer";
    case 0x0C:
        switch (s) {
        case 0x03: return "USB";
        case 0x05: return "SMBus";
        default:   return "soros busz";
        }
    default: return "egyeb";
    }
}
