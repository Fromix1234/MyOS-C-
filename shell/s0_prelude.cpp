// ===========================================================================
//  SHELL s0: globals, colours, draw primitives, font (draw_text)
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

// --- Цвета (настоящие RGB: 0xRRGGBB) — дизайн-система «Night Ridge» ---
const unsigned int C_BLACK  = 0x000000;
const unsigned int C_WHITE  = 0xF3F5F9;   // основной текст (почти белый)
const unsigned int C_RED    = 0xFF5F57;   // «светофор» окна: красная кнопка
const unsigned int C_YELLOW = 0xFEBC2E;   // ...жёлтая
const unsigned int C_GREEN  = 0x28C840;   // ...зелёная
// Цвета «стекла» (тусклые — они ложатся ПОВЕРХ фона полупрозрачно):
const unsigned int C_GLASS_TITLE  = 0x262A38;   // шапка окна
const unsigned int C_GLASS_BODY   = 0x0B0E17;   // тело окна (почти чёрное)
const unsigned int C_GLASS_DOCK   = 0x2A3040;   // плашка дока
const unsigned int C_GLASS_ICON   = 0x171A26;   // подложка иконки
const unsigned int C_GLASS_ICON_HV = 0x272C3C;  // ...под курсором (светлее)
const unsigned int C_GLASS_MBAR   = 0x141722;   // меню-бар
const unsigned int C_GLASS_TILE   = 0x232839;   // плитка Activities
// Рамки и выделение:
const unsigned int C_EDGE_F   = 0x8A92A6;  // контур окна В ФОКУСЕ
const unsigned int C_EDGE     = 0x4C5266;  // контур окна БЕЗ фокуса
const unsigned int C_ACCENT   = 0x9DB2FF;  // выделение (плитки, выбор в доке)
const unsigned int C_TEXT_DIM = 0x9BA2B3;  // вторичный текст/иконки статуса

// ===========================================================================
//  ОБОИ «NIGHT RIDGE» — ночной горный пейзаж (тёмная тема по концепту)
// ===========================================================================
//  wallpaper_pixel(x, y) — ЧИСТАЯ функция: для одних и тех же координат
//  всегда даёт один и тот же цвет. Ею рисуют обои и «стирают» временную
//  графику (курсор, подпись дока) — поэтому картинка никогда не «накапливает»
//  артефакты, и любые области восстанавливаются в точности.
//  Пейзаж (сверху вниз): небо (градиент + звёзды + луна с ореолом),
//  ДАЛЬНИЙ хребет (светлее — в дымке), СРЕДНИЙ и БЛИЖНИЙ (почти чёрный).
//  Всё на целочисленной математике: FPU в ядре отключён (-mno-80387).

// Рельеф хребта: треугольная волна 0..p с пиком в середине. Смешение двух
// волн с разными периодами даёт «ломаные» склоны без библиотеки sin/cos.
int wave_tri(int x, int p) {
    if (p < 2) return 0;
    x = x % p; if (x < 0) x += p;
    int t = (x << 8) / p;                   // место в периоде: 0..256
    return (t <= 128) ? t : 256 - t;        // подъём... затем спуск
}

// Смешать два цвета RGB (a = доля второго цвета 0..256)
unsigned int mix_col(unsigned int c1, unsigned int c2, int a) {
    if (a < 0) a = 0; if (a > 256) a = 256;
    int ia = 256 - a;
    int r = ((((c1 >> 16) & 0xFF) * ia + ((c2 >> 16) & 0xFF) * a) >> 8);
    int g = ((((c1 >> 8) & 0xFF) * ia + ((c2 >> 8) & 0xFF) * a) >> 8);
    int b = ((((c1) & 0xFF) * ia + ((c2) & 0xFF) * a) >> 8);
    return ((unsigned int)r << 16) | ((unsigned int)g << 8) | (unsigned int)b;
}

// Палитра пейзажа (тёмная тема):
const unsigned int RW_SKY_TOP = 0x06080F;   // глубокая тень в зените
const unsigned int RW_SKY_MID = 0x141A30;   // спокойная индго посередине
const unsigned int RW_SKY_HZ  = 0x3A3160;   // тёплый фиолет «заката»
const unsigned int RW_SKY_LOW = 0x1F1B3A;   // остывающая тень под закатом
const unsigned int RW_FAR     = 0x393D62;   // дальний хребет (в дымке)
const unsigned int RW_FAR_E   = 0x555C8E;   // ...его подсвеченный край
const unsigned int RW_MID     = 0x292E51;   // средний хребет
const unsigned int RW_MID_E   = 0x3B4269;   // ...его край
const unsigned int RW_NEAR    = 0x181C30;   // ближний хребет (почти чёрный)
const unsigned int RW_NEAR_E  = 0x262B47;   // ...его край
const unsigned int RW_MOON    = 0xE9ECF4;   // диск луны
const unsigned int RW_MOON_D  = 0xC3C9DC;   // полутень на диске
const unsigned int RW_HALO    = 0x4A4F76;   // цвет ореола

// Хребет layer (0=дальний..2=ближний) в точке x: y вершины силуэта.
// Периоды и амплитуды пропорциональны экрану — пейзаж верен при любом VESA.
int ridge_at(int x, int layer) {
    int h = (H > 0 ? H : 200), w = (W > 0 ? W : 320);
    if (layer == 0) {                      // дальний: пологий, высоко
        int base = h * 58 / 100;
        int y = base - (h * 6 / 100)  * wave_tri(x + 13, w / 6 + 2) / 128;
        y -= (h * 2 / 100) * wave_tri(x * 3 - 5, w / 19 + 2) / 128;
        return y;
    }
    if (layer == 1) {                      // средний: повыше, темнее
        int base = h * 65 / 100;
        int y = base - (h * 9 / 100)  * wave_tri(x + 7, w / 4 + 2) / 128;
        y -= (h * 3 / 100) * wave_tri(x * 2 + 91, w / 13 + 3) / 128;
        return y;
    }
    // ближний: доходит до краёв, самый тёмный
    int base = h * 73 / 100;
    int y = base - (h * 11 / 100) * wave_tri(x + 41, w / 3 + 3) / 128;
    y -= (h * 4 / 100) * wave_tri(x * 5 + 17, w / 7 + 2) / 128;
    return y;
}

// Небо для строки y: три опорные точки градиента. Ниже «заката» цвет
// остывает к тени — эта зона почти всегда закрыта хребтами.
unsigned int sky_color(int y) {
    int h = (H > 0 ? H : 200);
    int hi = h * 40 / 100;                      // до 40% — первая ветвь
    int hz = h * 58 / 100;                      // «линия заката»
    if (y <= hi) return mix_col(RW_SKY_TOP, RW_SKY_MID, y * 256 / (hi + 1));
    if (y <= hz) return mix_col(RW_SKY_MID, RW_SKY_HZ, (y - hi) * 256 / (hz - hi + 1));
    int d = y - hz; if (d > h * 20 / 100) d = h * 20 / 100;
    return mix_col(RW_SKY_HZ, RW_SKY_LOW, d * 256 / (h * 20 / 100 + 1));
}

// Звёзды: детерминированный хэш (узел звёзд одинаков при каждой перерисовке,
// иначе они бы «перемигивались» при частичном восстановлении областей).
unsigned int star_color(int x, int y) {
    unsigned int hh = (unsigned int)x * 73856093u ^ (unsigned int)y * 19349663u;
    hh ^= hh >> 13; hh *= 0x5BD1E995u; hh ^= hh >> 15;
    int m = (int)(hh % 97u);
    if (m > 3) return 0;                        // ~4% неба — звёзды
    return (m == 0) ? 0xC9D3EC : (m == 1) ? 0x8792B5 : 0x3C4462;
}

// Луна: диск с полутенью по горизонтали + мягкий ореол до 1.9 радиусов.
// Возвращает 0, если пиксель вне зоны луны.
unsigned int moon_color(int x, int y, unsigned int sky) {
    int h = (H > 0 ? H : 200), w = (W > 0 ? W : 320);
    int R = h / 24; if (R < 6) R = 6;
    int cx = w * 72 / 100, cy = h * 16 / 100;
    int dx = x - cx, dy = y - cy;
    if (dx > R * 2 || dx < -R * 2 || dy > R * 2 || dy < -R * 2) return 0; // зона
    int d2 = dx * dx + dy * dy, r2 = R * R;
    if (d2 <= r2) {                                    // сам диск
        int t = dx < 0 ? -dx * 110 / (R + 1) : dx * 110 / (R + 1);   // полутень
        return mix_col(RW_MOON, RW_MOON_D, t);
    }
    if (d2 <= r2 * 36 / 10) {                          // ореол
        int k = 256 - ((d2 - r2) * 256) / (r2 * 26 / 10 + 1);
        return mix_col(sky, RW_HALO, k * 3 / 4);       // мягкое свечение
    }
    return 0;
}

// ЦВЕТ ОБОЙ в произвольной точке экрана. Выбирается ближайший (к зрителю)
// слой, закрывающий точку; небо — только там, куда хребты не дотягиваются.
unsigned int wallpaper_pixel(int x, int y) {
    int y0 = ridge_at(x, 0), y1 = ridge_at(x, 1), y2 = ridge_at(x, 2);
    int layer = -1;
    if (y >= y2) layer = 2;
    else if (y >= y1) layer = 1;
    else if (y >= y0) layer = 0;
    if (layer < 0) {                                    // это небо
        unsigned int c = sky_color(y);
        if (y < (H * 48) / 100) {                       // звёзды — в верхней зоне
            unsigned int st = star_color(x, y);
            if (st) c = st;
        }
        unsigned int m = moon_color(x, y, c);           // луна поверх неба
        return m ? m : c;
    }
    static const unsigned int body[3] = { RW_FAR, RW_MID, RW_NEAR };
    static const unsigned int edge[3] = { RW_FAR_E, RW_MID_E, RW_NEAR_E };
    int topy = (layer == 2) ? y2 : (layer == 1) ? y1 : y0;
    if (y == topy || y == topy + 1)                     // блик по силуэту
        return edge[layer];
    return body[layer];
}
int iabs(int x) { return x < 0 ? -x : x; }   // модуль (свой, ведь stdlib нет)

// Собрать «машинное» значение пикселя из цвета 0xRRGGBB по маскам режима.
// Разные видео-BIOS кладут каналы по-разному (BGRX или RGBX) — VBE сообщил
// позиции битов R/G/B (см. «записку» stage2), и мы уважаем их. Поэтому
// цвета верны на ЛЮБОЙ видеокарте, а не только там, где автор угадал.
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

// Прочитать цвет 0xRRGGBB прямо из видеопамяти (нужно полупрозрачности)
unsigned int read_pixel(int x, int y) {
    if (x < 0 || x >= W || y < 0 || y >= H) return 0;
    unsigned char* p = fb + y * FB_PITCH + x * (FB_BPP / 8);
    unsigned int v;
    if (FB_BPP == 32) v = *(volatile unsigned int*)p;
    else v = (unsigned int)p[0] | ((unsigned int)p[1] << 8) | ((unsigned int)p[2] << 16);
    unsigned int r = (v >> FB_RPOS) & 0xFF;
    unsigned int g = (v >> FB_GPOS) & 0xFF;
    unsigned int b = (v >> FB_BPOS) & 0xFF;
    return (r << 16) | (g << 8) | b;
}

// Стереть область «до обоев» (например, подпись над доком) — тем же фоном.
// ВАЖНО: обои теперь пер-пиксельные (небо, звёзды, луна, хребты), поэтому
// восстановление идёт по одному пикселю через ту же функцию, что и рисует
// весь экран. Области совпадают ДО ТОЧНОГО пикселя, без «шрамов».
void erase_to_wallpaper(int x, int y, int w, int h) {
    for (int j = 0; j < h; j++) {
        int yy = y + j; if (yy < 0 || yy >= H) continue;
        for (int i = 0; i < w; i++) {
            int xx = x + i; if (xx < 0 || xx >= W) continue;
            put_pixel(xx, yy, wallpaper_pixel(xx, yy));
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

// Залить КРУГ (проверяем: расстояние до центра <= радиус)
void fill_circle(int cx, int cy, int r, unsigned int color) {
    int r2 = r * r;
    for (int dy = -r; dy <= r; dy++)
        for (int dx = -r; dx <= r; dx++)
            if (dx * dx + dy * dy <= r2)
                put_pixel(cx + dx, cy + dy, color);
}

// Нарисовать КОНТУР круга (пиксели, чьё расстояние ≈ радиус)
void draw_circle(int cx, int cy, int r, unsigned int color) {
    for (int dy = -r; dy <= r; dy++)
        for (int dx = -r; dx <= r; dx++) {
            int d2 = dx * dx + dy * dy;
            if (d2 >= r * r - r && d2 <= r * r + r)   // полоска вокруг окружности
                put_pixel(cx + dx, cy + dy, color);
        }
}

// Нарисовать ЛИНИЮ (алгоритм с фиксированной точкой — без float,
// потому что FPU в ядре отключён: -mno-80387)
void draw_line(int x1, int y1, int x2, int y2, unsigned int color) {
    int dx = x2 - x1, dy = y2 - y1;
    int steps = iabs(dx);
    if (iabs(dy) > steps) steps = iabs(dy);
    if (steps == 0) { put_pixel(x1, y1, color); return; }

    // работаем ×256 (8 дробных бит), чтобы не нужен float
    int x = x1 << 8, y = y1 << 8;
    int xinc = (dx << 8) / steps, yinc = (dy << 8) / steps;
    for (int i = 0; i <= steps; i++) {
        put_pixel(x >> 8, y >> 8, color);
        x += xinc;
        y += yinc;
    }
}
// ===========================================================================
//  ШРИФТ: рисование БУКВ пикселями (8x16, сгенерирован из Consolas)
// ===========================================================================
//  Глифы лежат в font_data.inc (сгенерирует font_gen.ps1). Символы ASCII
//  32..126, каждый = 16 байт = 16 строк. В одном байте — ряд из 8 пикселей:
//  бит 7 = левый пиксель, бит 0 = правый. Бит=1 -> ставим пиксель.
#include "../font_data.inc"             // v2.0: общий файл шрифта с ядром

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
