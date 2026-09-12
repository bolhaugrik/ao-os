#pragma once
#include "types.h"

struct pci_dev {
    u8  bus, dev, fn;
    u16 vendor, device;
    u8  class_, subclass, progif, rev;
    u8  header;
    u32 bar[6];
    u8  irq_line;
};

#define PCI_MAX 64

void pci_init(void);                 /* enumeracio, busz 0..7 */
u32  pci_count(void);
const struct pci_dev *pci_get(u32 i);
struct pci_dev *pci_get_mut(u32 i);
u32  pci_read32(u8 bus, u8 dev, u8 fn, u8 off);
u8   pci_read8(u8 bus, u8 dev, u8 fn, u8 off);
void pci_write8(u8 bus, u8 dev, u8 fn, u8 off, u8 v);
void pci_write32(u8 bus, u8 dev, u8 fn, u8 off, u32 v);
void pci_refresh(struct pci_dev *d);   /* osztaly/BAR ujraolvasasa */
const char *pci_class_name(u8 class_, u8 subclass);
