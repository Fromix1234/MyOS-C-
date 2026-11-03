#!/usr/bin/env python3
# ===========================================================================
#  mkdisk.py — собрать disk.img из готовых бинарников (без PowerShell)
# ===========================================================================
#  Повторяет раскладку из build.ps1 шаг в шаг, чтобы образ, собранный на
#  Linux/macOS/в CI, был БАЙТ В БАЙТ таким же, как на Windows:
#
#    0            boot.bin   (MBR, 512 байт)
#    1            vbr.bin    (VBR, 512 байт)
#    2..3         stage2.bin (дополнен до 1024 байт)
#    4..          kernel.bin
#
#    MBR-таблица: P1 = 2048, P2 = 18432, P3 = 34816 (по 16384 сектора)
#    Внутри P1:   +0 VBR (с LBA раздела в поле 504), +1..2 stage2, +3.. ядро
#    MyFS:        +192 суперблок, +193 таблица файлов, +194.. данные
#    shell.bin:   файл в MyFS и КОПИЯ на LBA 2560 (для загрузки с ISO)
#
#  Использование:
#    tools/mkdisk.py boot.bin vbr.bin stage2.bin kernel.bin shell.bin demo.bin \
#                    [-o disk.img] [-v]
# ===========================================================================
import sys

SECTOR = 512
DISK_SIZE = 32 * 1024 * 1024
DISK_SECTORS = DISK_SIZE // SECTOR
PART_RESERVED = 192
P1_LBA = 2048
PART_SECTORS = 16384
SHELL_BLOB_LBA = 2560

SHELL_BASE = 0x00400000


def ceildiv(a, b):
    return (a + b - 1) // b


def put16(buf, off, v):
    buf[off:off + 2] = (v & 0xFFFF).to_bytes(2, "little")


def put32(buf, off, v):
    buf[off:off + 4] = (v & 0xFFFFFFFF).to_bytes(4, "little")


def put_name(buf, off, name):
    raw = name.encode("ascii")
    assert len(raw) < 56, "имя файла MyFS длиннее 55 символов"
    buf[off:off + len(raw)] = raw


def write_at(img, lba, data):
    off = lba * SECTOR
    img[off:off + len(data)] = data


def build(boot, vbr, stage2, kernel, shell, demo):
    if len(boot) != SECTOR:
        raise SystemExit("boot.bin обязан быть 512 байт")
    if len(vbr) != SECTOR:
        raise SystemExit("vbr.bin обязан быть 512 байт")
    if len(stage2) > 1024:
        raise SystemExit("stage2.bin больше 1024 байт")
    kernel_sectors = ceildiv(len(kernel), SECTOR) + 1
    if kernel_sectors + 3 > PART_RESERVED:
        raise SystemExit("ядро не влезает в загрузочную зону раздела")
    if len(shell) > 0x100000:
        raise SystemExit("shell.bin больше 1 МБ (зона снимка ядра)")

    shell_secs = ceildiv(len(shell), SECTOR)
    if SHELL_BLOB_LBA + shell_secs > 2880:
        raise SystemExit("shell.bin не влезает в 1.44 МБ ISO-образа")

    img = bytearray(DISK_SIZE)
    img[0:len(boot)] = boot
    img[SECTOR:SECTOR + len(vbr)] = vbr
    img[2 * SECTOR:2 * SECTOR + len(stage2)] = stage2
    img[4 * SECTOR:4 * SECTOR + len(kernel)] = kernel

    # --- MBR --------------------------------------------------------------
    def part_entry(idx, flag, ptype, lba, sectors):
        off = 446 + idx * 16
        img[off] = flag
        img[off + 4] = ptype
        put32(img, off + 8, lba)
        put32(img, off + 12, sectors)

    part_entry(0, 0x80, 0x83, P1_LBA, PART_SECTORS)
    part_entry(1, 0x00, 0x83, P1_LBA + PART_SECTORS, PART_SECTORS)
    part_entry(2, 0x00, 0x83, P1_LBA + 2 * PART_SECTORS, PART_SECTORS)

    # --- загрузочная зона раздела P1 ---------------------------------------
    vbr_part = bytearray(vbr)
    put32(vbr_part, 504, P1_LBA)          # «записка» для stage2: LBA раздела
    write_at(img, P1_LBA, vbr_part)
    write_at(img, P1_LBA + 1, stage2[:SECTOR])
    write_at(img, P1_LBA + 2, stage2[SECTOR:1024])
    write_at(img, P1_LBA + 3, kernel)

    # --- MyFS --------------------------------------------------------------
    sb_lba = P1_LBA + PART_RESERVED
    tab_lba = sb_lba + 1
    data_lba = sb_lba + 2
    demo_start = data_lba + shell_secs
    demo_secs = ceildiv(len(demo), SECTOR)
    data_ptr = demo_start + demo_secs

    sb = bytearray(SECTOR)
    sb[0:4] = b"MYFS"
    put16(sb, 4, 1)                       # версия
    put32(sb, 6, PART_SECTORS)            # размер раздела в секторах
    put32(sb, 10, tab_lba)
    put32(sb, 14, data_lba)
    put32(sb, 18, data_ptr)
    put16(sb, 22, 2)                      # файлов
    put16(sb, 24, 8)                      # максимум записей
    write_at(img, sb_lba, sb)

    tab = bytearray(SECTOR)
    put_name(tab, 0, "shell.bin")
    put32(tab, 56, data_lba)
    put32(tab, 60, len(shell))
    put_name(tab, 64, "demo.bin")
    put32(tab, 120, demo_start)
    put32(tab, 124, len(demo))
    write_at(img, tab_lba, tab)

    write_at(img, data_lba, shell)
    write_at(img, demo_start, demo)
    write_at(img, SHELL_BLOB_LBA, shell)  # копия для ISO (El Torito)

    if SHELL_BLOB_LBA < data_ptr:
        raise SystemExit("копия shell.bin пересекается с данными MyFS")

    return img, {
        "kernel_sectors": kernel_sectors,
        "shell_sectors": shell_secs,
        "superblock_lba": sb_lba,
        "table_lba": tab_lba,
        "data_lba": data_lba,
        "data_ptr": data_ptr,
    }


def main(argv):
    args, out, verbose = [], "disk.img", False
    i = 0
    while i < len(argv):
        a = argv[i]
        if a == "-o":
            i += 1
            out = argv[i]
        elif a in ("-v", "--verbose"):
            verbose = True
        else:
            args.append(a)
        i += 1
    if len(args) != 6:
        print(__doc__ or "", file=sys.stderr)
        print("usage: mkdisk.py boot.bin vbr.bin stage2.bin kernel.bin shell.bin demo.bin [-o disk.img]",
              file=sys.stderr)
        return 2

    files = []
    for path in args:
        with open(path, "rb") as f:
            files.append(f.read())
    img, info = build(*files)
    with open(out, "wb") as f:
        f.write(img)
    if verbose:
        print("disk.img: %d bytes" % len(img))
        for k, v in info.items():
            print("  %-14s %s" % (k, v))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
