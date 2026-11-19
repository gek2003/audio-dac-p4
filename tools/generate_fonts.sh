#!/usr/bin/env bash
# ============================================================================
#  tools/generate_fonts.sh — генерация кириллических шрифтов для LVGL UI
#
#  Создаёт main/ui_font_ru_16.c, main/ui_font_ru_28.c, main/ui_font_ru_40.c
#  (Noto Sans, латиница + кириллица + пунктуация, 4bpp).
#
#  Требуется Node.js 16+ (npm). Запуск из корня проекта:
#      bash tools/generate_fonts.sh
#
#  Имена шрифтов (ui_font_ru_16/28/40) совпадают с weak-объявлениями в
#  main/ui.c — после генерации кириллица включается автоматически,
#  менять код не нужно. Не забудьте добавить файлы в SRCS main/CMakeLists.txt
#  (или перезаписать этот файл — в проекте они уже прописаны, см. ниже).
# ============================================================================
set -euo pipefail
cd "$(dirname "$0")/.."

if ! command -v node >/dev/null || ! command -v npm >/dev/null; then
    echo "ERROR: Node.js/npm не найдены. Установите Node.js LTS (https://nodejs.org)." >&2
    exit 1
fi

TTF=NotoSans-Regular.ttf
if [ ! -f "$TTF" ]; then
    echo "-- скачиваю Noto Sans Regular (кириллица есть)..."
    curl -L --fail -o "$TTF" \
      "https://raw.githubusercontent.com/notofonts/notofonts.github.io/main/fonts/NotoSans/hinted/ttf/NotoSans-Regular.ttf"
fi

# npm-пакет локально (без глобальной установки)
if [ ! -d .fonttools/node_modules/lv_font_conv ]; then
    echo "-- ставлю lv_font_conv..."
    mkdir -p .fonttools
    ( cd .fonttools && npm install lv_font_conv@latest --no-audit --no-fund )
fi
LFC=".fonttools/node_modules/lv_font_conv/lv_font_conv.js"

# Латиница + кириллица + тире/минус/кавычки/№
RANGE="0x20-0x7F,0x400-0x4FF,0x2010-0x2026,0x2116,0x2212,0xAB,0xBB"

gen () {
    SIZE=$1
    OUT=main/ui_font_ru_${SIZE}.c
    echo "-- генерирую $OUT..."
    node "$LFC" --font "$TTF" \
        --size "$SIZE" --bpp 4 --format lvgl \
        --force-fast-kern-format \
        --range "$RANGE" \
        -o "$OUT" \
        --lv-include lvgl.h \
        --lv-font-name "ui_font_ru_${SIZE}"
    echo "   $(wc -c < "$OUT") байт"
}

gen 16
gen 28
gen 40
echo "Готово. Файлы main/ui_font_ru_*.c добавьте в SRCS main/CMakeLists.txt"
