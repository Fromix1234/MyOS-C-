#!/usr/bin/env bash
# ===========================================================================
#  build.sh — сборка MyOS на Linux / macOS (аналог build.ps1)
# ===========================================================================
#  Собирает те же артефакты, что и Windows-скрипт:
#      kernel.bin  (ring 0, C++ kernel/ — или Rust при -r)
#      shell.bin   (ring 3, оболочка; tools/sim показывает её без загрузки)
#      demo.bin    (ring 3, тестовая программа для команды 'ring3')
#      disk.img    (32 МБ, MBR + MyFS, раскладка — tools/mkdisk.py)
#
#  Нужны: nasm, i686-ELF gcc/binutils и python3.
#    Debian/Ubuntu: sudo apt install nasm gcc-multilib binutils python3
#    Arch:          sudo pacman -S nasm gcc binutils python
#    macOS:         brew install nasm i686-elf-gcc i686-elf-binutils python3
#                   (или используйте build.ps1/MinGW; для UI — tools/sim)
#
#  Использование:
#      ./build.sh            # ядро C++ + оболочка + disk.img
#      ./build.sh -r         # ядро Rust (cargo +i686-unknown-linux-gnu, nightly)
#      ./build.sh -c         # сначала почистить артефакты
#
#  Переопределить инструменты: CC=... LD=... OBJCOPY=... NASM=... PYTHON=...
# ===========================================================================
set -euo pipefail

ROOT=$(cd "$(dirname "$0")" && pwd)
cd "$ROOT"

USE_RUST=0
CLEAN=0
for arg in "$@"; do
    case "$arg" in
        -r|--rust)  USE_RUST=1 ;;
        -c|--clean) CLEAN=1 ;;
        -h|--help)  sed -n '2,26p' "$0"; exit 0 ;;
        *) echo "unknown option: $arg" >&2; exit 2 ;;
    esac
done

need() {
    local tool=$1 hint=$2
    command -v "$tool" >/dev/null 2>&1 || { echo "ERROR: не найден $tool. $hint" >&2; exit 1; }
}

# --- выбор 32-битного компилятора -----------------------------------------
if [ -n "${CC:-}" ]; then
    :
elif command -v i686-elf-gcc >/dev/null 2>&1; then
    CC=i686-elf-gcc; LD=${LD:-i686-elf-ld}; OBJCOPY=${OBJCOPY:-i686-elf-objcopy}; NM=${NM:-i686-elf-nm}
elif command -v gcc >/dev/null 2>&1; then
    CC=gcc; LD=${LD:-ld}; OBJCOPY=${OBJCOPY:-objcopy}; NM=${NM:-nm}
else
    echo "ERROR: нет i686-elf-gcc или gcc" >&2; exit 1
fi
NASM=${NASM:-nasm}
PYTHON=${PYTHON:-python3}
need "$NASM" "Установите nasm"
need "$PYTHON" "Установите python3 (нужен для tools/mkdisk.py)"
need "$LD" "Установите binutils"
need "$OBJCOPY" "Установите binutils"

# Точка входа в скриптах линковщика записана с подчёркиванием (так её видит
# MinGW/COFF). В ELF имена C-функций без подчёркивания, поэтому подставляем
# алиасы, иначе ld не найдёт ENTRY(). Для MinGW алиасы не нужны.
# Алиасы раздельные: в объекте ядра нет shell_main, а в оболочке — kernel_main.
case "$CC" in
    *mingw*) DEFSYM_SHELL=() ;;
    *)       DEFSYM_SHELL=(--defsym _shell_main=shell_main) ;;
esac
if [ $USE_RUST -eq 1 ]; then
    DEFSYM_KERNEL=()          # Rust-ядро само определяет _kernel_main
else
    case "$CC" in
        *mingw*) DEFSYM_KERNEL=() ;;
        *)       DEFSYM_KERNEL=(--defsym _kernel_main=kernel_main) ;;
    esac
fi

if [ $CLEAN -eq 1 ]; then
    rm -f boot.bin vbr.bin stage2.bin kernel.bin kernel.elf kernel.o kernel_clean.o \
          disk.img user_demo.bin shell/shell.bin shell/shell.elf shell/shell.o shell/shell_clean.o
fi

CXXFLAGS=(-m32 -ffreestanding -fno-exceptions -fno-rtti -fno-pie
          -fno-stack-protector -nostdlib -nostartfiles -mno-sse -mno-mmx -mno-80387
          -fno-asynchronous-unwind-tables -Os -fno-strict-aliasing
          -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -Wno-unused-variable)

strip_obj() { "$OBJCOPY" --remove-section=.comment --remove-section=.note \
                          --remove-section=.note.GNU-stack "$1" "$2"; }

# --- 0. ring 3 демка -------------------------------------------------------
echo "=== 0. user_demo.asm ==="
"$NASM" -f bin user_demo.asm -o user_demo.bin

# --- 1. оболочка -----------------------------------------------------------
echo "=== 1. shell -> shell.bin ==="
"$CC" "${CXXFLAGS[@]}" -c shell/shell.cpp -o shell/shell.o
strip_obj shell/shell.o shell/shell_clean.o
"$LD" -m elf_i386 "${DEFSYM_SHELL[@]}" -T shell/shell.ld -o shell/shell.elf shell/shell_clean.o
"$OBJCOPY" -O binary shell/shell.elf shell/shell.bin
SHELL_SIZE=$(wc -c < shell/shell.bin | tr -d '[:space:]')
if [ "$SHELL_SIZE" -gt 1048576 ]; then
    echo "ERROR: shell.bin больше 1 МБ (зона снимка ядра)" >&2; exit 1
fi
SHELL_ENTRY=$("$NM" shell/shell.elf | awk '$3=="shell_main"{print $1; exit}')
echo "    shell.bin = $SHELL_SIZE bytes (entry 0x${SHELL_ENTRY:-?})"
if [ -n "${SHELL_ENTRY:-}" ] && [ "$SHELL_ENTRY" != "00400000" ]; then
    echo "    WARN: shell_main по адресу 0x$SHELL_ENTRY, ожидался 0x00400000" >&2
fi

# --- 2. ядро ---------------------------------------------------------------
if [ $USE_RUST -eq 1 ]; then
    echo "=== 2. Rust kernel (kernel-rs) ==="
    need cargo "Установите Rust (rustup)"
    ( cd kernel-rs && cargo +nightly build --release --target i686-unknown-linux-gnu \
                  -Z build-std=core )
    RUST_LIB=kernel-rs/target/i686-unknown-linux-gnu/release/libmyos_kernel_rs.a
    [ -f "$RUST_LIB" ] || { echo "ERROR: нет $RUST_LIB" >&2; exit 1; }
    "$LD" -m elf_i386 --gc-sections "${DEFSYM_KERNEL[@]}" -T kernel-rs/kernel.ld -o kernel.elf "$RUST_LIB"
    "$OBJCOPY" -O binary kernel.elf kernel.bin
else
    echo "=== 2. kernel -> kernel.bin ==="
    SECTORS=16
    for _pass in 1 2 3; do
        "$CC" "${CXXFLAGS[@]}" -DKERNEL_SECTORS=$SECTORS -c kernel/kernel.cpp -o kernel.o
        strip_obj kernel.o kernel_clean.o
        "$LD" -m elf_i386 "${DEFSYM_KERNEL[@]}" -T kernel.ld -o kernel.elf kernel_clean.o
        "$OBJCOPY" -O binary kernel.elf kernel.bin
        KSIZE=$(wc -c < kernel.bin | tr -d '[:space:]')
        NEWSECTORS=$(( (KSIZE + 511) / 512 + 1 ))
        [ "$NEWSECTORS" -eq "$SECTORS" ] && break
        SECTORS=$NEWSECTORS
    done
fi
KERNEL_SIZE=$(wc -c < kernel.bin | tr -d '[:space:]')
KERNEL_SECTORS=$(( (KERNEL_SIZE + 511) / 512 + 1 ))
echo "    kernel.bin = $KERNEL_SIZE bytes = $KERNEL_SECTORS sectors"
if [ $(( KERNEL_SECTORS + 3 )) -gt 192 ]; then
    echo "ERROR: ядро не влезает в загрузочную зону (PART_RESERVED=192)" >&2; exit 1
fi
KENTRY=$("$NM" kernel.elf | awk '$3=="kernel_main"{print $1; exit}')
echo "    kernel_main = 0x${KENTRY:-?}"

# --- 3. загрузчик ----------------------------------------------------------
echo "=== 3. bootloader ==="
"$NASM" -f bin boot.asm -o boot.bin
"$NASM" -f bin vbr.asm  -o vbr.bin
"$NASM" -f bin -dKERNEL_ENTRY=0x${KENTRY:-10000} -dKERNEL_SECTORS=$KERNEL_SECTORS \
    -dSHELL_BLOB_LBA=2560 -dSHELL_BLOB_SECTORS=$(( (SHELL_SIZE + 511) / 512 )) \
    -dSHELL_BLOB_BYTES=$SHELL_SIZE stage2.asm -o stage2.bin
[ "$(wc -c < boot.bin)" -eq 512 ]   || { echo "ERROR: boot.bin не 512 байт" >&2; exit 1; }
[ "$(wc -c < vbr.bin)"  -eq 512 ]   || { echo "ERROR: vbr.bin не 512 байт" >&2; exit 1; }
[ "$(wc -c < stage2.bin)" -le 1024 ] || { echo "ERROR: stage2.bin больше 1024 байт" >&2; exit 1; }

# --- 4. образ диска --------------------------------------------------------
echo "=== 4. disk.img ==="
"$PYTHON" tools/mkdisk.py boot.bin vbr.bin stage2.bin kernel.bin shell/shell.bin user_demo.bin -o disk.img -v
echo "Done: disk.img (32 MB). Запуск: VirtualBox (VDI) или tools/sim для интерфейса."
