#!/usr/bin/env python3
# ===========================================================================
#  mkiso.py — собрать загрузочный MyOS.iso без внешних утилит
# ===========================================================================
#  Аналог build_iso.ps1 для Linux/macOS. Создаёт ISO9660 + El Torito
#  (эмуляция 1.44 MB floppy) с корректными полями PVD, поэтому образ
#  читается операционной системой и при этом загружается BIOS/UEFI-CSM.
#
#  Раскладка:
#    0..15   system area (нули)
#    16      Primary Volume Descriptor (ISO 9660)
#    17      Boot Record Volume Descriptor (El Torito)
#    18      Boot Catalog
#    19      Volume Descriptor Set Terminator
#    20      root directory
#    21      README.TXT
#    22      Type-L path table
#    23      Type-M path table
#    32..    boot image: первые 1.44 MB disk.img (720 x 2048-byte sectors)
#
#  Загрузочный образ — это дискета 1.44 MB. BIOS даёт MBR настоящий int 13h,
#  поэтому цепочка MBR -> VBR -> stage2 -> kernel работает без изменений.
#  stage2 берёт shell.bin с LBA 2560 (внутри этих 1.44 MB), так что ядру
#  не нужен ни ATA-, ни CD-драйвер.
#
#  Использование:
#    tools/mkiso.py [disk.img] [-o MyOS.iso] [--boot-image boot.img]
#                   [--version "MyOS 3.2"] [--volume-id MYOS_V32]
# ===========================================================================
import argparse
import datetime as _dt
import struct
import sys
from pathlib import Path

SECTOR = 2048
FLOPPY_BYTES = 1_474_560              # 1.44 MB = 2880 x 512
FLOPPY_SECTORS = FLOPPY_BYTES // SECTOR  # 720

PVD_LBA = 16
BRVD_LBA = 17
CATALOG_LBA = 18
TERM_LBA = 19
ROOT_LBA = 20
README_LBA = 21
PATHL_LBA = 22
PATHM_LBA = 23
BOOT_LBA = 32
TOTAL_SECTORS = BOOT_LBA + FLOPPY_SECTORS  # 752


def put_le16(buf, off, value):
    struct.pack_into("<H", buf, off, value & 0xFFFF)


def put_be16(buf, off, value):
    struct.pack_into(">H", buf, off, value & 0xFFFF)


def put_le32(buf, off, value):
    struct.pack_into("<I", buf, off, value & 0xFFFFFFFF)


def put_be32(buf, off, value):
    struct.pack_into(">I", buf, off, value & 0xFFFFFFFF)


def put_both16(buf, off, value):
    put_le16(buf, off, value)
    put_be16(buf, off + 2, value)


def put_both32(buf, off, value):
    put_le32(buf, off, value)
    put_be32(buf, off + 4, value)


def put_ascii(buf, off, text):
    raw = text.encode("ascii")
    buf[off:off + len(raw)] = raw


def fill_spaces(buf, off, count):
    buf[off:off + count] = b" " * count


def dir_record(extent, size, is_dir, ident, when):
    """Directory record по ECMA-119 9.1 (с паддингом до чётной длины)."""
    length = 33 + len(ident)
    if length % 2:
        length += 1
    rec = bytearray(length)
    rec[0] = length
    rec[1] = 0                       # extended attribute record length
    put_both32(rec, 2, extent)
    put_both32(rec, 10, size)
    rec[18] = when.year - 1900
    rec[19] = when.month
    rec[20] = when.day
    rec[21] = when.hour
    rec[22] = when.minute
    rec[23] = when.second
    rec[24] = 0                      # GMT offset, 15-min units
    rec[25] = 0x02 if is_dir else 0x00
    rec[26] = 0                      # file unit size
    rec[27] = 0                      # interleave gap
    put_both16(rec, 28, 1)           # volume sequence number
    rec[32] = len(ident)
    rec[33:33 + len(ident)] = ident
    return bytes(rec)


def pvd_datetime(when):
    """17-байтовая дата PVD:YYYYMMDDHHMMSScc + GMT offset."""
    stamp = when.strftime("%Y%m%d%H%M%S").encode("ascii") + b"00"
    return stamp + b"\x00"           # UTC


def build_iso(disk_path, iso_path, boot_path, version, volume_id):
    disk = disk_path.read_bytes()
    if len(disk) < FLOPPY_BYTES:
        raise SystemExit(f"ERROR: {disk_path} меньше 1.44 MB")
    boot = disk[:FLOPPY_BYTES]
    boot_path.write_bytes(boot)
    print(f"boot.img = {FLOPPY_BYTES} bytes (1.44 MB floppy image)")

    now = _dt.datetime.now(_dt.timezone.utc).replace(tzinfo=None)
    iso = bytearray(TOTAL_SECTORS * SECTOR)

    def put_sector(src, lba):
        iso[lba * SECTOR:lba * SECTOR + SECTOR] = src

    # --- Primary Volume Descriptor (ECMA-119 8.4) -------------------------
    pvd = bytearray(SECTOR)
    pvd[0] = 1
    put_ascii(pvd, 1, "CD001")
    pvd[6] = 1
    fill_spaces(pvd, 8, 32)          # system identifier
    put_ascii(pvd, 8, "MYOS")
    fill_spaces(pvd, 40, 32)         # volume identifier
    put_ascii(pvd, 40, volume_id)
    put_both32(pvd, 80, TOTAL_SECTORS)   # volume space size
    put_both16(pvd, 120, 1)              # volume set size
    put_both16(pvd, 124, 1)              # volume sequence number
    put_both16(pvd, 128, SECTOR)         # logical block size
    put_both32(pvd, 132, 10)             # path table size
    put_le32(pvd, 140, PATHL_LBA)
    put_le32(pvd, 144, 0)                # optional Type-L path table
    put_le32(pvd, 148, PATHM_LBA)
    put_le32(pvd, 152, 0)                # optional Type-M path table

    root_rec = dir_record(ROOT_LBA, SECTOR, True, b"\x00", now)
    pvd[156:156 + len(root_rec)] = root_rec

    for off in (190, 318, 446, 574):
        fill_spaces(pvd, off, 128)
    for off in (702, 739, 776):
        fill_spaces(pvd, off, 37)

    put_ascii(pvd, 190, volume_id)
    put_ascii(pvd, 318, "CEO FROMIX")
    put_ascii(pvd, 446, "MyOS project")
    put_ascii(pvd, 574, version)

    stamp = pvd_datetime(now)
    for off in (813, 830, 847, 864):
        pvd[off:off + 17] = stamp
    pvd[881] = 1                     # file structure version
    put_sector(pvd, PVD_LBA)

    # --- Boot Record Volume Descriptor (El Torito) ------------------------
    brvd = bytearray(SECTOR)
    brvd[0] = 0
    put_ascii(brvd, 1, "CD001")
    brvd[6] = 1
    put_ascii(brvd, 7, "EL TORITO SPECIFICATION")
    put_le32(brvd, 71, CATALOG_LBA)
    put_sector(brvd, BRVD_LBA)

    # --- Volume Descriptor Set Terminator ---------------------------------
    term = bytearray(SECTOR)
    term[0] = 255
    put_ascii(term, 1, "CD001")
    term[6] = 1
    put_sector(term, TERM_LBA)

    # --- Boot Catalog -----------------------------------------------------
    cat = bytearray(SECTOR)
    cat[0] = 1                       # validation entry
    cat[1] = 0                       # 80x86
    put_ascii(cat, 4, version[:28])
    checksum = (-sum(struct.unpack_from("<16H", cat, 0))) & 0xFFFF
    put_le16(cat, 28, checksum)
    cat[30] = 0x55
    cat[31] = 0xAA

    cat[32] = 0x88                   # bootable
    cat[33] = 0x02                   # 1.44 MB floppy emulation
    put_le16(cat, 34, 0)             # load segment
    cat[36] = 0
    cat[37] = 0
    put_le16(cat, 38, 1)             # sector count
    put_le32(cat, 40, BOOT_LBA)
    put_sector(cat, CATALOG_LBA)

    # --- Root directory ---------------------------------------------------
    readme_text = (
        f"{version}\r\n"
        "Design by CEO FROMIX\r\n"
        "You can be on the list of developers.\r\n"
        "WARNING!!! Project now in pre-release\r\n"
        "\r\n"
        "Bootable ISO: El Torito 1.44 MB floppy emulation.\r\n"
        "The hard-disk image (MyOS.vdi) is shipped separately in the release.\r\n"
    )
    readme = readme_text.encode("ascii")

    root = bytearray(SECTOR)
    entries = b"".join((
        dir_record(ROOT_LBA, SECTOR, True, b"\x00", now),
        dir_record(ROOT_LBA, SECTOR, True, b"\x01", now),
        dir_record(README_LBA, len(readme), False, b"README.TXT;1", now),
        dir_record(BOOT_LBA, FLOPPY_BYTES, False, b"BOOT.IMG;1", now),
    ))
    root[:len(entries)] = entries
    put_sector(root, ROOT_LBA)

    readme_sector = bytearray(SECTOR)
    readme_sector[:len(readme)] = readme
    put_sector(readme_sector, README_LBA)

    # --- Path tables (по одной записи — корень) ---------------------------
    path_l = bytearray(SECTOR)
    path_l[0] = 1
    path_l[1] = 0
    put_le32(path_l, 2, ROOT_LBA)
    put_le16(path_l, 6, 1)
    path_l[8] = 0
    put_sector(path_l, PATHL_LBA)

    path_m = bytearray(SECTOR)
    path_m[0] = 1
    path_m[1] = 0
    put_be32(path_m, 2, ROOT_LBA)
    put_be16(path_m, 6, 1)
    path_m[8] = 0
    put_sector(path_m, PATHM_LBA)

    # --- Boot image at fixed LBA ------------------------------------------
    iso[BOOT_LBA * SECTOR:BOOT_LBA * SECTOR + FLOPPY_BYTES] = boot

    iso_path.write_bytes(iso)
    print(f"ISO created: {iso_path} ({len(iso)} bytes, {TOTAL_SECTORS} sectors)")
    print(f"Boot image: El Torito 1.44 MB floppy emulation at LBA {BOOT_LBA}")


def main():
    ap = argparse.ArgumentParser(description="Build MyOS bootable ISO (ISO9660 + El Torito)")
    ap.add_argument("disk", nargs="?", default="disk.img", type=Path)
    ap.add_argument("-o", "--output", default="MyOS.iso", type=Path)
    ap.add_argument("--boot-image", default="boot.img", type=Path)
    ap.add_argument("--version", default="MyOS 3.2")
    ap.add_argument("--volume-id", default="MYOS_V32")
    args = ap.parse_args()

    if not args.disk.is_file():
        raise SystemExit(f"ERROR: {args.disk} not found. Run ./build.sh first.")
    build_iso(args.disk, args.output, args.boot_image, args.version, args.volume_id)


if __name__ == "__main__":
    main()
