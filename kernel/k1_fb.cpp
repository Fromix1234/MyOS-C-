// ===========================================================================
//  KERNEL k1: framebuffer primitives + font (splash text)
// ===========================================================================
//  MyOS v2.0 module. Split mechanically from kernel.cpp v1.2
//  (git a81d4f4) and then reworked: see README section 11.

// --- Графические переменные (значения приходят из «записки» stage2) ---
int W = 320;                             // ширина экрана в пикселях
int H = 200;                             // высота экрана в пикселях
int FB_PITCH = 320;                      // байт в одной строке видеопамяти
int FB_BPP = 8;                          // бит на пиксель (24 или 32)
int FB_RPOS = 16, FB_GPOS = 8, FB_BPOS = 0;   // где в пикселе биты R/G/B
                                         // (по умолчанию классический BGRX)
unsigned char* fb = (unsigned char*)0xA0000;  // видеопамять (кадровый буфер)

// --- Цвета (настоящие RGB: 0xRRGGBB) ---
const unsigned int C_BLACK  = 0x000000;
const unsigned int C_WHITE  = 0xFFFFFF;
const unsigned int C_RED    = 0xFF3B30;  // «светофор» окна: красная кнопка
const unsigned int C_YELLOW = 0xFFCC00;  // ...жёлтая
const unsigned int C_GREEN  = 0x34C759;  // ...зелёная
// Цвета «стекла» (тусклые — они ложатся ПОВЕРХ фона полупрозрачно):
const unsigned int C_GLASS_TITLE = 0x2A3140;   // шапка окна
const unsigned int C_GLASS_DOCK  = 0x3A4150;   // плашка дока
const unsigned int C_GLASS_ICON  = 0x1E2028;   // подложка иконки
unsigned int encode_color(unsigned int c) {
    unsigned int r = (c >> 16) & 0xFF, g = (c >> 8) & 0xFF, b = c & 0xFF;
    return (r << FB_RPOS) | (g << FB_GPOS) | (b << FB_BPOS);
}

// Поставить ОДИН пиксель. Проверяем границы, чтобы не вылезти за память!
// Цвет пишем напрямую: 32 бита = 4 байта, 24 бита = 3 байта (по маскам VBE).
void put_pixel(int x, int y, unsigned int color) {
    if (x < 0 || x >= W || y < 0 || y >= H) return;
    unsigned char* p = fb + y * FB_PITCH + x * (FB_BPP / 8);
    unsigned int v = encode_color(color);
    if (FB_BPP == 32) {
        *(volatile unsigned int*)p = v;
    } else {
        p[0] = v & 0xFF;
        p[1] = (v >> 8) & 0xFF;
        p[2] = (v >> 16) & 0xFF;
    }
}

// Залить прямоугольник цветом. Пишем не по одному пикселю, а СРАЗУ ЦЕЛЫМИ
// строками: при 32 битах на пиксель — командой rep stosl (процессор сам
// повторяет запись слова), поэтому фон, окна и очистка областей ускорились
// в разы. Границы отсекаются один раз, а не в каждой точке.
void fill_rect(int x, int y, int w, int h, unsigned int color) {
    if (w <= 0 || h <= 0) return;
    int x0 = x < 0 ? 0 : x,  y0 = y < 0 ? 0 : y;
    int x1 = x + w, y1 = y + h;
    if (x1 > W) x1 = W;
    if (y1 > H) y1 = H;
    if (x0 >= x1 || y0 >= y1) return;
    unsigned int v = encode_color(color);
    int n = x1 - x0;                              // сколько пикселей в строке
    if (FB_BPP == 32) {
        unsigned int* row = (unsigned int*)(fb + y0 * FB_PITCH) + x0;
        int step = FB_PITCH >> 2;                 // шаг строки в 32-битных словах
        for (int j = y0; j < y1; j++, row += step) {
            // rep stosl: процессор сам повторяет запись слова (ECX раз по 4 байта).
            // «+D» и «+c» — ЧЕСТНО говорим компилятору, что EDI и ECX изменяются
            // (иначе он счёл бы их нетронутыми и испортил бы свои переменные).
            unsigned int* d = row;
            int cnt = n;
            asm volatile("cld; rep stosl" : "+D"(d), "+c"(cnt) : "a"(v) : "memory");
        }
    } else {                                      // 24 бита: три байта на пиксель
        unsigned char b0 = (unsigned char)(v & 0xFF);
        unsigned char b1 = (unsigned char)((v >> 8) & 0xFF);
        unsigned char b2 = (unsigned char)((v >> 16) & 0xFF);
        for (int j = y0; j < y1; j++) {
            unsigned char* p = fb + j * FB_PITCH + x0 * 3;
            for (int i = 0; i < n; i++, p += 3) { p[0] = b0; p[1] = b1; p[2] = b2; }
        }
    }
}
// Нарисовать КОНТУР прямоугольника (только рамка)
void draw_rect(int x, int y, int w, int h, unsigned int color) {
    for (int i = 0; i < w; i++) {              // верх и низ
        put_pixel(x + i, y, color);
        put_pixel(x + i, y + h - 1, color);
    }
    for (int j = 0; j < h; j++) {              // лево и право
        put_pixel(x, y + j, color);
        put_pixel(x + w - 1, y + j, color);
    }
}
// ===========================================================================
//  ШРИФТ: рисование БУКВ пикселями (8x16, сгенерирован из Consolas)
// ===========================================================================
//  Глифы лежат в font_data.inc (сгенерирует font_gen.ps1). Символы ASCII
//  32..126, каждый = 16 байт = 16 строк. В одном байте — ряд из 8 пикселей:
//  бит 7 = левый пиксель, бит 0 = правый. Бит=1 -> ставим пиксель.
#include "../font_data.inc"

// Цвет обычного текста (тот же белый, что TEXT_COL оболочки): заставка,
// экран паники — чтобы код k9 совпадал с оболочкой один в один.
const unsigned int TEXT_COL = C_WHITE;

// Прочитать «записку» stage2 о видеорежиме VESA VBE и запомнить её.
// Делает ТОЛЬКО ядро (в реальном режиме режим ставил stage2); оболочка потом
// спросит уже готовые значения системным вызовом SYS_FB.
void init_video() {
    if (*(volatile unsigned short*)0x0510 != 0x4D56) return;  // записки нет
    unsigned int   fba   = *(volatile unsigned int*)0x0504;
    unsigned short pitch = *(volatile unsigned short*)0x0508;
    unsigned short w     = *(volatile unsigned short*)0x050A;
    unsigned short h     = *(volatile unsigned short*)0x050C;
    unsigned char  bpp   = *(volatile unsigned char*)0x050E;
    unsigned char  rp    = *(volatile unsigned char*)0x0512;
    unsigned char  gp    = *(volatile unsigned char*)0x0513;
    unsigned char  bp    = *(volatile unsigned char*)0x0514;
    if (fba && pitch && w && h && (bpp == 24 || bpp == 32)) {
        fb = (unsigned char*)fba;        // всё правдоподобно — принимаем
        FB_PITCH = pitch; FB_BPP = bpp; W = w; H = h;
        if (rp < 32 && gp < 32 && bp < 32 && (rp != gp || gp != bp)) {
            FB_RPOS = rp; FB_GPOS = gp; FB_BPOS = bp;   // порядок каналов VBE
        }
    }
}

// Нарисовать ОДИН символ в точке (x, y)
void draw_char(int x, int y, char c, unsigned int color) {
    if (c < 32 || c > 126) return;              // вне шрифта — пропуск
    const unsigned char* glyph = font8x16[c - 32];   // адрес глифа (16 байт)
    for (int row = 0; row < 16; row++) {
        unsigned char bits = glyph[row];         // один ряд = 8 пикселей
        for (int col = 0; col < 8; col++) {
            if (bits & (0x80 >> col))            // если бит включён
                put_pixel(x + col, y + row, color);
        }
    }
}

// Нарисовать строку: символ за символом, шаг вправо на 8 пикселей
void draw_text(int x, int y, const char* s, unsigned int color) {
    while (*s) {
        draw_char(x, y, *s, color);
        x += 8;       // ширина знакоместа
        s++;          // следующий символ
    }
}
