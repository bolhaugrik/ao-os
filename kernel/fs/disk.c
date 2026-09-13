#include "disk.h"
#include "vfs.h"
#include "aofs.h"
#include "aofs2.h"
#include "bootinfo.h"
#include "layout.h"
#include "syscall.h"
#include "../drv/blk.h"
#include "../mm/kheap.h"
#include "../lib/string.h"
#include "../lib/fmt.h"
#include "../shell/shell.h"

extern struct bootinfo *boot_info;   /* kmain adja */

bool disk_find_partition(u64 *start, u64 *sectors)
{
    if (!blk_present()) return false;
    u8 mbr[512];
    if (blk_read(0, 1, mbr)) return false;
    if (mbr[510] != 0x55 || mbr[511] != 0xAA) return false;
    for (int i = 0; i < 4; i++) {
        const u8 *e = mbr + 0x1BE + i * 16;
        if (e[4] != 0x7F) continue;
        u32 s = e[8] | (e[9] << 8) | (e[10] << 16) | ((u32)e[11] << 24);
        u32 n = e[12] | (e[13] << 8) | (e[14] << 16) | ((u32)e[15] << 24);
        if (!s || !n) continue;
        *start = s;
        *sectors = n;
        return true;
    }
    return false;
}

int disk_mount_root(void)
{
    u64 start, n;
    if (!disk_find_partition(&start, &n)) return E_NOENT;
    bool dirty = false;
    void *fs = aofs2_mount(start, n, &dirty);
    if (!fs) return E_INVAL;
    /* a ramdisk /-rol /rd-re koltozik, a lemez lesz a gyoker */
    vfs_unmount("/");
    vfs_mount("/", &aofs2_ops, fs, false);
    if (aofs_mounted())
        vfs_mount("/rd", &aofs1_ops, NULL, true);
    u32 blocks, freeb, inodes;
    char label[32];
    aofs2_info(fs, &blocks, &freeb, &inodes, label, sizeof label);
    kprintf("lemez: AOFS v2 '%s' LBA %lu, %u blokk (%u szabad), %u inode%s, csatolva: /\n",
            label, start, blocks, freeb, inodes, dirty ? ", PISZKOS -> bitmap ujraszamolva" : "");
    return 0;
}

/* fajl a ramdiskbol: /rd/<p> vagy /<p> (attol fuggoen, mi a gyoker) */
static int rd_read(const char *p, void **buf, usize *size)
{
    char path[VFS_PATH_MAX];
    snformat(path, sizeof path, "/rd/%s", p);
    if (vfs_read_all(path, buf, size) == 0) return 0;
    snformat(path, sizeof path, "/%s", p);
    return vfs_read_all(path, buf, size);
}

static int copy_tree(const char *src_dir, const char *dst_dir, bool quiet)
{
    struct dirent *ents = kmalloc(64 * sizeof *ents);
    int n = vfs_list(src_dir, ents, 64);
    if (n < 0) { kfree(ents); return n; }
    for (int i = 0; i < n; i++) {
        char s[VFS_PATH_MAX], d[VFS_PATH_MAX];
        snformat(s, sizeof s, "%s/%s", strcmp(src_dir, "/") == 0 ? "" : src_dir, ents[i].name);
        snformat(d, sizeof d, "%s/%s", strcmp(dst_dir, "/") == 0 ? "" : dst_dir, ents[i].name);
        if (ents[i].type == 2) {
            if (strcmp(ents[i].name, "rd") == 0 || strcmp(ents[i].name, "tmp") == 0 || strcmp(ents[i].name, "sys") == 0)
                continue;
            int e = vfs_mkdir(d);
            if (e && e != E_EXIST) { kfree(ents); return e; }
            e = copy_tree(s, d, quiet);
            if (e) { kfree(ents); return e; }
        } else {
            void *buf;
            usize size;
            int e = vfs_read_all(s, &buf, &size);
            if (e) { kfree(ents); return e; }
            e = vfs_write_all(d, buf, size);
            kfree(buf);
            if (e) { kfree(ents); return e; }
            if (!quiet) kprintf("  %s (%lu B)\n", d, (u64)size);
        }
    }
    kfree(ents);
    return 0;
}

/* ---------------------------------------------------------------- build-belyeg */
#define STAMP_OFF ((u64)64 * 512 - 64)      /* a stage2-terulet utolso 64 bajtja (tools/mkimage.py) */
#define STAMP_MAX 64

/* egy szovegfajl elso sora (ures, ha nincs) */
static void read_first_line(const char *path, char *out, usize cap)
{
    out[0] = 0;
    void *b;
    usize n;
    if (vfs_read_all(path, &b, &n)) return;
    const char *p = b;
    usize l = 0;
    while (l < n && l + 1 < cap && p[l] != '\n' && p[l] != '\r') { out[l] = p[l]; l++; }
    out[l] = 0;
    kfree(b);
}

void disk_running_build(char *out, usize cap)
{
    /* a ramdisk /rd alatt van, ha a lemez a gyoker; kulonben o maga a gyoker */
    read_first_line("/rd/etc/build", out, cap);
    if (!out[0] && !aofs_mounted()) return;
    if (!out[0]) read_first_line("/etc/build", out, cap);
}

void disk_image_stamp(const void *img, usize n, char *out, usize cap)
{
    out[0] = 0;
    if (n < STAMP_OFF + STAMP_MAX) return;
    const char *p = (const char *)img + STAMP_OFF;
    usize l = 0;
    while (l < STAMP_MAX && l + 1 < cap && p[l] >= ' ' && p[l] < 127) { out[l] = p[l]; l++; }
    out[l] = 0;
}

bool disk_booted_from_disk(void)
{
    if (!blk_present() || !boot_info->ramdisk_size) return false;
    u8 sec[512];
    if (blk_read(1, 1, sec) || memcmp(sec + 4, "AOS2", 4) != 0) return false;
    u32 hdr[4];
    memcpy(hdr, sec + 8, sizeof hdr);
    u32 r_lba = hdr[2], r_sect = hdr[3];
    if (!r_sect || r_sect != (boot_info->ramdisk_size + 511) / 512) return false;
    const u8 *rd = P2V(boot_info->ramdisk_paddr);
    if (blk_read(r_lba, 1, sec) || memcmp(sec, rd, 512) != 0) return false;
    u8 last[512];
    memset(last, 0, sizeof last);
    usize off = (usize)(r_sect - 1) * 512;
    memcpy(last, rd + off, boot_info->ramdisk_size - off);
    return blk_read(r_lba + r_sect - 1, 1, sec) == 0 && memcmp(sec, last, 512) == 0;
}

/* Boot utan, ha a lemez a gyoker: a ramdisk build-belyege mas, mint a lemez /etc/build-je -> a bin/ es etc/
 * frissul a ramdiskbol (a halozati update igy juttatja a programokat a lemezre). Csak akkor, ha a lemez
 * boot-terulete ezt a ramdiskt tartalmazza: egy regi pendrive-rol indulva nem irjuk felul a lemezt. */
void disk_sync_from_ramdisk(void)
{
    char rd[STAMP_MAX], dk[STAMP_MAX];
    read_first_line("/rd/etc/build", rd, sizeof rd);
    read_first_line("/etc/build", dk, sizeof dk);
    if (!rd[0] || strcmp(rd, dk) == 0) return;
    if (!disk_booted_from_disk()) {
        kprintf("lemez: mas build fut (%s), a lemezen %s; nem a lemezrol indult, a programok maradnak (update)\n",
                rd, dk[0] ? dk : "nincs belyeg");
        return;
    }
    kprintf("lemez: frissites a ramdiskbol: %s -> %s\n", dk[0] ? dk : "nincs belyeg", rd);
    int e = copy_tree("/rd", "/", true);
    vfs_sync();
    if (e) kprintf("lemez: a frissites megszakadt (hiba %d); update rd a pendrive-rol\n", e);
    else kprintf("[frissitve: bin/ es etc/ a ramdiskbol, build %s]\n", rd);
}

/* ---------------------------------------------------------------- halozati frissites */
int disk_update_image(const void *img, usize n)
{
    if (!blk_present()) return E_IO;
    const u8 *p = img;
    if (n < 65 * 512 || n > (usize)PANIC_LBA * 512 || (n & 511)) { kprintf("update: rossz kepmeret (%lu B)\n", (u64)n); return E_INVAL; }
    if (p[510] != 0x55 || p[511] != 0xAA || memcmp(p + 512 + 4, "AOS2", 4) != 0) { kprintf("update: nem AO-boot-kep\n"); return E_INVAL; }
    u32 hdr[4];
    memcpy(hdr, p + 512 + 8, sizeof hdr);
    u32 k_lba = hdr[0], k_sect = hdr[1], r_lba = hdr[2], r_sect = hdr[3];
    u32 end = r_sect ? r_lba + r_sect : k_lba + k_sect;
    if (k_lba != 64 || !k_sect || (r_sect && r_lba != 64 + k_sect) || end > n / 512 || end >= PANIC_LBA) {
        kprintf("update: rossz fejlec (kernel %u+%u, ramdisk %u+%u)\n", k_lba, k_sect, r_lba, r_sect);
        return E_INVAL;
    }
    if (r_sect) {
        const struct aofs_super *sb = (const struct aofs_super *)(p + (u64)r_lba * 512);
        if (sb->magic != AOFS_MAGIC || sb->version != 1 || sb->total_size > r_sect * 512) { kprintf("update: rossz ramdisk a kepben\n"); return E_INVAL; }
    }
    u64 ps, pn;
    if (!disk_find_partition(&ps, &pn)) { kprintf("update: nincs AO-particio, hasznald az install-t\n"); return E_NOENT; }
    u8 mbr[512];
    if (blk_read(0, 1, mbr)) return E_IO;
    memcpy(mbr, p, 0x1BE);                  /* uj boot-kod, a lemez sajat particios tablaja marad */
    vfs_sync();

    /* 1. kernel + ramdisk, darabonkent, visszaolvasva; 2. stage2; 3. legvegul az MBR */
    kprintf("update: kernel (%u szektor) + ramdisk (%u szektor) irasa...\n", k_sect, r_sect);
    u8 *chk = kmalloc(128 * 512);
    int e = 0;
    for (u32 lba = 64; lba < end && !e; lba += 128) {
        u32 cnt = end - lba > 128 ? 128 : end - lba;
        const u8 *src = p + (u64)lba * 512;
        if ((e = blk_write(lba, cnt, src))) break;
        if ((e = blk_read(lba, cnt, chk))) break;
        if (memcmp(chk, src, (usize)cnt * 512) != 0) { kprintf("update: visszaolvasasi hiba LBA %u-nal\n", lba); e = E_IO; }
    }
    kfree(chk);
    if (e) return e;
    if ((e = blk_write(1, 63, p + 512))) return e;
    if ((e = blk_write(0, 1, mbr))) return e;
    blk_flush();
    return 0;
}

int disk_mkfs(const char *label)
{
    if (!blk_present()) return E_IO;
    u64 total = blk_sectors();
    if (total < PART_LBA + 4096) return E_INVAL;
    u64 psect = total - PART_LBA;
    if (psect > 0xFFFFFFFFULL) psect = 0xFFFFFFFFULL;
    return aofs2_mkfs(PART_LBA, psect, label);
}

int disk_install(const char *label)
{
    return disk_install_ex(label, false);
}

int disk_update(void)
{
    return disk_install_ex(NULL, true);
}

/* keep_fs: nincs formazas, a particio es a /state megmarad; a boot-terulet es a
 * ramdisk fajljai (bin/, etc/) felulirodnak */
int disk_install_ex(const char *label, bool keep_fs)
{
    if (!blk_present()) return E_IO;
    void *s1, *s2, *kern;
    usize s1n, s2n, kn;
    int e;
    if ((e = rd_read("boot/stage1.bin", &s1, &s1n))) { kprintf("install: nincs boot/stage1.bin a ramdiskben\n"); return e; }
    if ((e = rd_read("boot/stage2.bin", &s2, &s2n))) { kfree(s1); kprintf("install: nincs boot/stage2.bin\n"); return e; }
    if ((e = rd_read("boot/kernel.bin", &kern, &kn))) { kfree(s1); kfree(s2); kprintf("install: nincs boot/kernel.bin\n"); return e; }
    if (s1n != 512 || memcmp((u8 *)s2 + 4, "AOS2", 4) != 0) { kfree(s1); kfree(s2); kfree(kern); return E_INVAL; }

    u64 total = blk_sectors();
    u64 psect = total - PART_LBA;
    if (psect > 0xFFFFFFFFULL) psect = 0xFFFFFFFFULL;
    u32 k_sect = (u32)((kn + 511) / 512);
    u32 r_lba = 64 + k_sect;
    u32 r_sect = (boot_info->ramdisk_size + 511) / 512;
    if (r_lba + r_sect >= PANIC_LBA) { kfree(s1); kfree(s2); kfree(kern); kprintf("install: kernel+ramdisk nem fer a foglalt teruletre\n"); return E_LIMIT; }

    kprintf("%s: lemez '%s', %lu MiB, particio LBA %u (%lu MiB)\n", keep_fs ? "update" : "install",
            blk_model(), total / 2048, PART_LBA, psect / 2048);

    /* 1. particio formazasa (update-nel kihagyva) */
    if (!keep_fs) {
        kprintf("install: AOFS v2 formazas...\n");
        e = aofs2_mkfs(PART_LBA, psect, label);
        if (e) { kfree(s1); kfree(s2); kfree(kern); return e; }
    } else {
        u64 ps, pn;
        if (!disk_find_partition(&ps, &pn)) { kfree(s1); kfree(s2); kfree(kern); kprintf("update: nincs AO-particio, hasznald az install-t\n"); return E_NOENT; }
        vfs_sync();
    }

    /* 2. stage2 fejlec: kernel/ramdisk LBA */
    u8 *st2 = kmalloc(63 * 512);
    memset(st2, 0, 63 * 512);
    memcpy(st2, s2, s2n);
    u32 *hdr = (u32 *)(st2 + 8);
    hdr[0] = 64; hdr[1] = k_sect; hdr[2] = r_sect ? r_lba : 0; hdr[3] = r_sect;
    {   /* build-belyeg a stage2-terulet vegere (mint a mkimage-nel), a ramdisk /etc/build-jebol */
        char stamp[STAMP_MAX];
        disk_running_build(stamp, sizeof stamp);
        memcpy(st2 + STAMP_OFF - 512, stamp, strlen(stamp));
    }

    /* 3. MBR: particios tabla */
    u8 mbr[512];
    memcpy(mbr, s1, 512);
    u8 *pe = mbr + 0x1BE;
    pe[0] = 0x80; pe[4] = 0x7F;
    pe[8] = PART_LBA & 0xFF; pe[9] = (PART_LBA >> 8) & 0xFF; pe[10] = (PART_LBA >> 16) & 0xFF; pe[11] = (PART_LBA >> 24) & 0xFF;
    pe[12] = psect & 0xFF; pe[13] = (psect >> 8) & 0xFF; pe[14] = (psect >> 16) & 0xFF; pe[15] = (psect >> 24) & 0xFF;

    kprintf("install: bootloader + kernel (%u szektor) + ramdisk (%u szektor)...\n", k_sect, r_sect);
    u8 *kpad = kmalloc((usize)k_sect * 512);
    memset(kpad, 0, (usize)k_sect * 512);
    memcpy(kpad, kern, kn);
    if ((e = blk_write(64, k_sect, kpad))) goto out;
    if (r_sect) {
        u8 *rpad = kmalloc((usize)r_sect * 512);
        memset(rpad, 0, (usize)r_sect * 512);
        memcpy(rpad, P2V(boot_info->ramdisk_paddr), boot_info->ramdisk_size);
        e = blk_write(r_lba, r_sect, rpad);
        kfree(rpad);
        if (e) goto out;
    }
    if ((e = blk_write(1, 63, st2))) goto out;
    if ((e = blk_write(0, 1, mbr))) goto out;
    u8 zero[512];
    memset(zero, 0, sizeof zero);
    blk_write(PANIC_LBA, 1, zero);
    blk_flush();

    /* 4. csatolas es a ramdisk tartalmanak masolasa */
    kprintf("%s: fajlok masolasa...\n", keep_fs ? "update" : "install");
    if (!keep_fs || !aofs_mounted() || vfs_mount_at(0) == NULL || strcmp(vfs_mount_at(0)->ops->name, "aofs2") != 0) {
        e = disk_mount_root();
        if (e) goto out;
    }
    e = copy_tree("/rd", "/", false);
    if (e) goto out;
    vfs_mkdir("/state");
    vfs_mkdir("/state/agents");
    vfs_mkdir("/project");
    vfs_sync();
    if (keep_fs) kprintf("update: kesz. Inditsd ujra a gepet (reboot).\n");
    else kprintf("install: kesz. Kihuzhatod a pendrive-ot, a gep a belso lemezrol indul.\n");
out:
    kfree(kpad);
    kfree(st2);
    kfree(s1);
    kfree(s2);
    kfree(kern);
    return e;
}

/* ---------------------------------------------------------------- panic-tarolo */
#define PANIC_MAGIC "AOPANIC1"

void panic_store_write(const char *text)
{
    if (!blk_present()) return;
    u8 sec[512];
    memset(sec, 0, sizeof sec);
    memcpy(sec, PANIC_MAGIC, 8);
    usize n = strlen(text);
    if (n > 500) n = 500;
    memcpy(sec + 8, text, n);
    blk_write(PANIC_LBA, 1, sec);
    blk_flush();
}

int panic_store_read(char *buf, usize cap)
{
    if (!blk_present()) return 0;
    u8 sec[512];
    if (blk_read(PANIC_LBA, 1, sec)) return 0;
    if (memcmp(sec, PANIC_MAGIC, 8) != 0) return 0;
    usize n = 0;
    while (n < 500 && sec[8 + n] && n + 1 < cap) { buf[n] = (char)sec[8 + n]; n++; }
    buf[n] = 0;
    return (int)n;
}

void panic_store_clear(void)
{
    if (!blk_present()) return;
    u8 sec[512];
    memset(sec, 0, sizeof sec);
    blk_write(PANIC_LBA, 1, sec);
}
