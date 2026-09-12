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

static int copy_tree(const char *src_dir, const char *dst_dir)
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
            e = copy_tree(s, d);
            if (e) { kfree(ents); return e; }
        } else {
            void *buf;
            usize size;
            int e = vfs_read_all(s, &buf, &size);
            if (e) { kfree(ents); return e; }
            e = vfs_write_all(d, buf, size);
            kfree(buf);
            if (e) { kfree(ents); return e; }
            kprintf("  %s (%lu B)\n", d, (u64)size);
        }
    }
    kfree(ents);
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

    kprintf("install: lemez '%s', %lu MiB, particio LBA %u (%lu MiB)\n", blk_model(), total / 2048, PART_LBA, psect / 2048);

    /* 1. particio formazasa */
    kprintf("install: AOFS v2 formazas...\n");
    e = aofs2_mkfs(PART_LBA, psect, label);
    if (e) { kfree(s1); kfree(s2); kfree(kern); return e; }

    /* 2. stage2 fejlec: kernel/ramdisk LBA */
    u8 *st2 = kmalloc(63 * 512);
    memset(st2, 0, 63 * 512);
    memcpy(st2, s2, s2n);
    u32 *hdr = (u32 *)(st2 + 8);
    hdr[0] = 64; hdr[1] = k_sect; hdr[2] = r_sect ? r_lba : 0; hdr[3] = r_sect;

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
    kprintf("install: fajlok masolasa...\n");
    e = disk_mount_root();
    if (e) goto out;
    e = copy_tree("/rd", "/");
    if (e) goto out;
    vfs_mkdir("/state");
    vfs_mkdir("/state/agents");
    vfs_mkdir("/project");
    vfs_sync();
    kprintf("install: kesz. Kihuzhatod a pendrive-ot, a gep a belso lemezrol indul.\n");
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
