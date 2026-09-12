/* ACPI: csak annyi, amennyi a kikapcsolashoz kell (RSDP -> RSDT -> FADT -> DSDT _S5). */
#pragma once
#include "types.h"

bool acpi_init(void);          /* tablak megkeresese; false ha nincs ACPI */
bool acpi_available(void);
void acpi_poweroff(void);      /* ha sikerul, nem ter vissza */
u64  acpi_rsdp_paddr(void);
