#include "acpi.h"
#include "layout.h"
#include "../arch/io.h"
#include "../cpu/tsc.h"
#include "../lib/string.h"

struct rsdp {
    char sig[8];
    u8 checksum;
    char oem[6];
    u8 rev;
    u32 rsdt;
    u32 length;
    u64 xsdt;
    u8 ext_checksum;
    u8 res[3];
} PACKED;

struct sdt_header {
    char sig[4];
    u32 length;
    u8 rev, checksum;
    char oem[6], oem_table[8];
    u32 oem_rev, creator, creator_rev;
} PACKED;

struct fadt {
    struct sdt_header h;
    u32 firmware_ctrl, dsdt;
    u8 res0, pm_profile;
    u16 sci_int;
    u32 smi_cmd;
    u8 acpi_enable, acpi_disable, s4bios, pstate;
    u32 pm1a_evt, pm1b_evt, pm1a_cnt, pm1b_cnt;
} PACKED;

static u64 rsdp_pa;
static u32 pm1a_cnt, pm1b_cnt, smi_cmd;
static u8 acpi_enable;
static u16 slp_typa, slp_typb;
static bool ready;

static bool checksum_ok(const void *p, usize n)
{
    u8 s = 0;
    for (usize i = 0; i < n; i++) s = (u8)(s + ((const u8 *)p)[i]);
    return s == 0;
}

static const struct rsdp *find_rsdp(void)
{
    /* EBDA elso KiB-je, majd 0xE0000..0xFFFFF, 16 bajtos lepesekben */
    u16 ebda_seg = *(const u16 *)P2V(0x40E);
    u64 ebda = (u64)ebda_seg << 4;
    u64 ranges[2][2] = { { ebda, ebda + 1024 }, { 0xE0000, 0x100000 } };
    for (int r = 0; r < 2; r++) {
        if (!ranges[r][0]) continue;
        for (u64 a = ranges[r][0]; a + sizeof(struct rsdp) <= ranges[r][1]; a += 16) {
            const struct rsdp *p = P2V(a);
            if (memcmp(p->sig, "RSD PTR ", 8) == 0 && checksum_ok(p, 20)) {
                rsdp_pa = a;
                return p;
            }
        }
    }
    return NULL;
}

/* _S5_ csomag keresese a DSDT AML-jeben: "_S5_" 0x12 pkglen numelems [0x0A] typa [0x0A] typb */
static bool parse_s5(const u8 *aml, u32 len)
{
    for (u32 i = 0; i + 8 < len; i++) {
        if (memcmp(aml + i, "_S5_", 4) != 0) continue;
        const u8 *p = aml + i + 4;
        if (*p != 0x12) continue;               /* PackageOp */
        p++;
        u32 pl = *p & 0x3F;                     /* PkgLength: 1..4 bajt */
        u32 extra = *p >> 6;
        p += 1 + extra;
        (void)pl;
        p++;                                    /* elemszam */
        u16 v[2];
        for (int k = 0; k < 2; k++) {
            if (*p == 0x0A) { v[k] = p[1]; p += 2; }        /* BytePrefix */
            else if (*p == 0x0B) { v[k] = (u16)(p[1] | (p[2] << 8)); p += 3; }  /* WordPrefix */
            else if (*p == 0x00) { v[k] = 0; p++; }
            else if (*p == 0x01) { v[k] = 1; p++; }
            else return false;
        }
        slp_typa = v[0];
        slp_typb = v[1];
        return true;
    }
    return false;
}

bool acpi_init(void)
{
    const struct rsdp *r = find_rsdp();
    if (!r || !r->rsdt) return false;
    const struct sdt_header *rsdt = P2V(r->rsdt);
    if (memcmp(rsdt->sig, "RSDT", 4) != 0) return false;
    u32 n = (rsdt->length - sizeof *rsdt) / 4;
    const u32 *ents = (const u32 *)((const u8 *)rsdt + sizeof *rsdt);
    const struct fadt *f = NULL;
    for (u32 i = 0; i < n; i++) {
        const struct sdt_header *h = P2V(ents[i]);
        if (memcmp(h->sig, "FACP", 4) == 0) { f = (const struct fadt *)h; break; }
    }
    if (!f) return false;
    pm1a_cnt = f->pm1a_cnt;
    pm1b_cnt = f->pm1b_cnt;
    smi_cmd = f->smi_cmd;
    acpi_enable = f->acpi_enable;
    if (!f->dsdt) return false;
    const struct sdt_header *dsdt = P2V(f->dsdt);
    if (memcmp(dsdt->sig, "DSDT", 4) != 0) return false;
    if (!parse_s5((const u8 *)dsdt + sizeof *dsdt, dsdt->length - sizeof *dsdt)) return false;
    ready = true;
    return true;
}

bool acpi_available(void) { return ready; }
u64 acpi_rsdp_paddr(void) { return rsdp_pa; }

void acpi_poweroff(void)
{
    if (!ready || !pm1a_cnt) return;
    /* ACPI mod bekapcsolasa, ha a BIOS meg legacy modban hagyta */
    if (smi_cmd && acpi_enable && !(inw((u16)pm1a_cnt) & 1)) {
        outb((u16)smi_cmd, acpi_enable);
        u64 end = rdtsc() + tsc_hz() * 3;
        while (!(inw((u16)pm1a_cnt) & 1) && rdtsc() < end) ;
    }
    outw((u16)pm1a_cnt, (u16)((slp_typa << 10) | (1 << 13)));
    if (pm1b_cnt)
        outw((u16)pm1b_cnt, (u16)((slp_typb << 10) | (1 << 13)));
    /* ha ide jutunk, nem sikerult */
}
