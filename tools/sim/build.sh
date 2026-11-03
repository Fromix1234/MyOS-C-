#!/usr/bin/env bash
# ===========================================================================
#  build.sh — сборка ХОСТОВОГО СИМУЛЯТОРА оболочки MyOS (macOS/Linux)
# ===========================================================================
#  Настоящий код оболочки (shell/*.cpp) собирается обычным clang++ и рисует
#  кадр в обычную память. Нужен только для разработки интерфейса: посмотреть
#  рабочий стол, не поднимая VirtualBox.
#
#  Готовый образ ОС собирается ТОЛЬКО через build.ps1 (MinGW i686 + NASM);
#  этот скрипт к нему отношения не имеет и в диск не попадает.
#
#  Использование:
#      tools/sim/build.sh                         # собрать tools/sim/build/sim
#      tools/sim/build.sh 1440x900 shot.bmp       # + отрисовать кадр
#      tools/sim/build/sim 1024x768 a.bmp open:0  # запуск без пересборки
# ===========================================================================
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
OUT="$HERE/build"
SRC="$OUT/root"

rm -rf "$OUT/root"
mkdir -p "$SRC/shell"

# Копия исходников оболочки: unity-сборка берёт файлы по относительным путям
# (#include "../common/myos.h"), поэтому повторяем раскладку репозитория.
cp "$ROOT"/shell/*.cpp "$SRC/shell/"
ln -sfn "$ROOT/common"         "$SRC/common"
ln -sfn "$ROOT/font_data.inc"  "$SRC/font_data.inc"
ln -sfn "$ROOT/icons_data.inc" "$SRC/icons_data.inc"

# 32-битная оболочка хранит указатели в int (сисколлы int 0x80). На 64-битном
# хосте такой каст — ошибка компиляции, поэтому в КОПИИ исходников расширяем
# его до long. Оригинальные файлы не трогаем.
if sed --version >/dev/null 2>&1; then
    SEDI=(sed -i -E)
else
    SEDI=(sed -i '' -E)
fi
"${SEDI[@]}" 's/\(int\)/(long)/g' "$SRC"/shell/*.cpp
"${SEDI[@]}" 's/\(unsigned int\)/(unsigned long)/g' "$SRC"/shell/*.cpp
# .text.entry — атрибут формата PE (его понимает ld из MinGW); на Mach-O/ELF
# секция задаётся иначе, а для симулятора это не нужно вовсе.
"${SEDI[@]}" 's/__attribute__\(\(section\(".text.entry"\)\)\)//g' "$SRC"/shell/*.cpp

# -fno-strict-aliasing: оболочка пишет в кадровый буфер через разные типы.
# -w: в оригинале много осмысленных сужений (int -> char) — шум не нужен.
clang++ -std=c++17 -O1 -w \
    -fno-exceptions -fno-rtti -fno-strict-aliasing \
    -DMYOS_HOST_SIM=1 \
    -I"$ROOT" \
    -o "$OUT/sim" \
    "$SRC/shell/shell.cpp" "$HERE/sim_main.cpp"

echo "built: $OUT/sim"

# Необязательный снимок: build.sh WxH file.bmp [сценарий...]
if [ "$#" -ge 1 ]; then
    SIZE=${1:-1024x768}
    SHOT=${2:-desktop.bmp}
    shift 2 2>/dev/null || true
    "$OUT/sim" "$SIZE" "$SHOT" "$@" >/dev/null
    if command -v sips >/dev/null 2>&1; then      # macOS: BMP -> PNG
        sips -s format png "$SHOT" --out "${SHOT%.bmp}.png" >/dev/null
        echo "frame: ${SHOT%.bmp}.png"
    else
        echo "frame: $SHOT"
    fi
fi
