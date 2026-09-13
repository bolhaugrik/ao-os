#!/usr/bin/env python3
"""AO-OS lemezkep-epito.

Elrendezes (LBA, 512 bajtos szektor):
  0          stage1 (MBR + particios tabla)
  1..63      stage2
  64..       kernel (lapos binaris)
  utana      ramdisk (opcionalis, szektorra igazitva)
  2048-tol   AOFS particio (Phase 1-tol; most ures)

A stage2 fejleceben ('AOS2' utan) a kernel es ramdisk LBA/szektorszam kerul
felulirasra, a stage1 particios tablajaban a particio kezdete es merete.
"""
import argparse
import struct
import sys

SECTOR = 512
STAGE2_LBA = 1
STAGE2_MAX_SECTORS = 63
KERNEL_LBA = 64
PART_ALIGN = 2048


def sectors(n_bytes):
    return (n_bytes + SECTOR - 1) // SECTOR


def pad(b, n):
    return b + b"\0" * (n - len(b))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--stage1", required=True)
    ap.add_argument("--stage2", required=True)
    ap.add_argument("--kernel", required=True)
    ap.add_argument("--ramdisk")
    ap.add_argument("--fs", help="AOFS particio-kep (opcionalis)")
    ap.add_argument("--size-mib", type=int, default=8, help="teljes kep merete MiB-ban (min.)")
    ap.add_argument("-o", "--output", required=True)
    a = ap.parse_args()

    stage1 = open(a.stage1, "rb").read()
    stage2 = open(a.stage2, "rb").read()
    kernel = open(a.kernel, "rb").read()
    ramdisk = open(a.ramdisk, "rb").read() if a.ramdisk else b""
    fs = open(a.fs, "rb").read() if a.fs else b""

    if len(stage1) != SECTOR:
        sys.exit(f"stage1 merete {len(stage1)}, 512-nek kell lennie")
    if stage1[510:512] != b"\x55\xAA":
        sys.exit("stage1: hianyzik a 0x55AA alairas")
    if sectors(len(stage2)) > STAGE2_MAX_SECTORS:
        sys.exit(f"stage2 tul nagy: {len(stage2)} bajt (max {STAGE2_MAX_SECTORS * SECTOR})")
    if stage2[4:8] != b"AOS2":
        sys.exit("stage2: hianyzik az AOS2 fejlec")

    k_sect = sectors(len(kernel))
    r_lba = KERNEL_LBA + k_sect
    r_sect = sectors(len(ramdisk))
    if r_lba + r_sect > PART_ALIGN - 1:
        sys.exit(f"hiba: a boot-terulet megtelt (kernel + ramdisk = {r_lba + r_sect} szektor > {PART_ALIGN - 1}); nagy programok a share/ mappaba (ao.py)")
    end_lba = r_lba + r_sect
    part_lba = ((end_lba + PART_ALIGN - 1) // PART_ALIGN) * PART_ALIGN
    if part_lba == 0:
        part_lba = PART_ALIGN

    total_sect = max(a.size_mib * 1024 * 1024 // SECTOR, part_lba + sectors(len(fs)) + PART_ALIGN)
    part_sect = total_sect - part_lba

    # stage2 fejlec: kernel_lba, kernel_sectors, ramdisk_lba, ramdisk_sectors
    stage2 = bytearray(pad(stage2, STAGE2_MAX_SECTORS * SECTOR))
    struct.pack_into("<IIII", stage2, 8, KERNEL_LBA, k_sect, r_lba if r_sect else 0, r_sect)

    # stage1 particios tabla, 1. bejegyzes: LBA kezdet (+8), meret (+12)
    stage1 = bytearray(stage1)
    struct.pack_into("<II", stage1, 0x1BE + 8, part_lba, part_sect)

    img = bytearray(total_sect * SECTOR)
    img[0:SECTOR] = stage1
    img[STAGE2_LBA * SECTOR:(STAGE2_LBA + STAGE2_MAX_SECTORS) * SECTOR] = stage2
    img[KERNEL_LBA * SECTOR:KERNEL_LBA * SECTOR + len(kernel)] = kernel
    if ramdisk:
        img[r_lba * SECTOR:r_lba * SECTOR + len(ramdisk)] = ramdisk
    if fs:
        img[part_lba * SECTOR:part_lba * SECTOR + len(fs)] = fs

    with open(a.output, "wb") as f:
        f.write(img)

    print(f"image: {a.output}  {len(img) // 1024} KiB")
    print(f"  stage2  LBA {STAGE2_LBA:>6}  {len(open(a.stage2,'rb').read()):>8} B")
    print(f"  kernel  LBA {KERNEL_LBA:>6}  {len(kernel):>8} B  ({k_sect} szektor)")
    if ramdisk:
        print(f"  ramdisk LBA {r_lba:>6}  {len(ramdisk):>8} B  ({r_sect} szektor)")
    print(f"  aofs    LBA {part_lba:>6}  {part_sect * SECTOR // 1024:>8} KiB particio")


if __name__ == "__main__":
    main()
