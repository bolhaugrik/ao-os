#pragma once
#include "types.h"
#include "pci.h"

bool ahci_init(void);                 /* PCI-keresés, HBA + elso lemezes port inicializalasa */
bool amd_sata_to_ahci(struct pci_dev *d);   /* AMD SB7x0/Hudson: IDE-mod -> AHCI-mod */
u32  ahci_port_index(void);
const char *ahci_error(void);
u64  ahci_abar(void);
