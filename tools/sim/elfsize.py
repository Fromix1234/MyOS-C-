#!/usr/bin/env python3
"""elfsize.py — размер ELF-объекта по секциям (для бюджета shell.bin).

Apple-утилиты (size/objdump) читают только Mach-O, а оболочка собирается
кросс-компилятором в ELF32/PE. Этот скрипт парсит заголовки секций сам.

    python3 tools/sim/elfsize.py build/shell32.o
"""
import struct
import sys


def main(path):
    with open(path, 'rb') as f:
        data = f.read()
    if data[:4] != b'\x7fELF':
        sys.exit('not an ELF file: ' + path)
    is64 = data[4] == 2
    if is64:
        e_shoff, = struct.unpack_from('<Q', data, 0x28)
        e_shentsize, e_shnum, e_shstrndx = struct.unpack_from('<HHH', data, 0x3A)
        sh_fmt = '<IIQQQQIIQQ'
    else:
        e_shoff, = struct.unpack_from('<I', data, 0x20)
        e_shentsize, e_shnum, e_shstrndx = struct.unpack_from('<HHH', data, 0x2E)
        sh_fmt = '<IIIIIIIIII'

    def section(i):
        off = e_shoff + i * e_shentsize
        return struct.unpack_from(sh_fmt, data, off)

    strtab = section(e_shstrndx)
    names_off = strtab[4]

    def name(idx):
        start = names_off + idx
        end = data.index(b'\0', start)
        return data[start:end].decode()

    total = 0
    rows = []
    for i in range(e_shnum):
        sh = section(i)
        nm = name(sh[0])
        flags = sh[2]
        if flags & 0x2 and not nm.startswith('.rel'):   # SHF_ALLOC
            rows.append((nm, sh[5]))
            total += sh[5]
    width = max(len(n) for n, _ in rows) if rows else 0
    for nm, sz in rows:
        print(f'{nm:<{width}}  {sz:>7} bytes')
    print(f'{"TOTAL":<{width}}  {total:>7} bytes')


if __name__ == '__main__':
    main(sys.argv[1] if len(sys.argv) > 1 else 'shell32.o')
