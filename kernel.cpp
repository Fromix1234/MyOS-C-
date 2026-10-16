// ===========================================================================
//  kernel.cpp — ядро MyOS: ГРАФИКА (VESA VBE) + примитивы рисования
// ===========================================================================
//  Видеорежим ставит stage2.asm через VESA VBE («универсальный драйвер») и
//  оставляет ядру «записку» о видео в низкой памяти (0x0504...0x0510):
//    адрес видеопамяти, pitch (байт в строке), ширина, высота, бит/пиксель.
//  Пиксель — НАСТОЯЩИЙ цвет 0xRRGGBB (не индекс палитры!):
//    чёрный = 0x000000, белый = 0xFFFFFF, красный = 0xFF3B30 и т.д.
//
//  Здесь построен ФУНДАМЕНТ графической оболочки:
//    * put_pixel / fill_rect / draw_line / fill_circle  — кирпичи для окон
//    * цвет пишется напрямую в видеопамять (24 или 32 бита на пиксель)
// ===========================================================================

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

// Цвет фона («рабочего стола») для строки y — очень мягкий градиент:
// сверху тёмно-синий, книзу чёрный. Он ПОЧТИ чёрный (белый текст читается
// идеально), но по нему ВИДНО стекло: полупрозрачные док и шапки окон
// слегка подсвечиваются фоном. Функция чистая: её можно вызвать в любом
// месте экрана, чтобы ВОССТАНОВИТЬ фон (стекло, скролл, очистка областей).
unsigned int wallpaper(int y) {
    int t = (y * 44) / (H > 0 ? H : 1);           // 44 сверху .. 0 снизу
    unsigned int v = (unsigned int)(44 - t);
    return ((v / 2) << 16) | ((v * 3 / 4) << 8) | v;
}

// ===========================================================================
//  ПРИМИТИВЫ РИСОВАНИЯ (база для будущих окон и кнопок)
// ===========================================================================
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

// Стереть область «до обоев» (например, подпись над доком) — тем же фоном
void erase_to_wallpaper(int x, int y, int w, int h) {
    for (int j = y; j < y + h; j++) {
        if (j < 0 || j >= H) continue;
        fill_rect(x, j, w, 1, wallpaper(j));
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
#include "font_data.inc"
#include "user_demo_data.inc"            // программа ring 3 (собрал build.ps1)

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

// ===========================================================================
//  ПОРТЫ ВВОДА-ВЫВОДА (общение с железом напрямую)
// ===========================================================================
void outb(unsigned short port, unsigned char value) {
    asm volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

// Прочитать байт из порта
unsigned char inb(unsigned short port) {
    unsigned char result;
    asm volatile("inb %1, %0" : "=a"(result) : "Nd"(port));
    return result;
}

// Прочитать СЛОВО (16 бит) из порта — нужно диску ATA: данные идут по 16 бит
unsigned short inw(unsigned short port) {
    unsigned short result;
    asm volatile("inw %1, %0" : "=a"(result) : "Nd"(port));
    return result;
}

// Записать СЛОВО (16 бит) в порт — тоже для диска ATA (данные идут по 16 бит)
void outw(unsigned short port, unsigned short value) {
    asm volatile("outw %0, %1" : : "a"(value), "Nd"(port));
}

// ===========================================================================
//  ЦВЕТА: раньше здесь была палитра VGA (порты 0x3C8/0x3C9, 6 бит канал).
//  Теперь цвета — настоящие RGB (см. C_BLACK/C_WHITE/... в начале файла),
//  а режим VESA пишет их в видеопамять напрямую. Палитра больше не нужна.
// ===========================================================================

// ===========================================================================
//  «СТЕКЛЯННОЕ» ОКНО (glassmorphism) — главный элемент графической оболочки
// ===========================================================================
//  Идея: вместо сплошной заливки точечно ставим чёрный поверх СВЕЖИХ обоев.
//  Часть пикселей остаётся обоями (просвечивают), часть — цвет стекла.
//  Так получается полупрозрачность БЕЗ float и БЕЗ пер-пиксельного alpha
//  (FPU в ядре отключён). Чем больше dens — тем плотнее стекло.
const unsigned char bayer4[16] = {       // матрица дизеринга 4x4 (значения 0..15)
     0,  8,  2, 10,
    12,  4, 14,  6,
     3, 11,  1,  9,
    15,  7, 13,  5
};

// Идёт ПОЛНЫЙ кадр (компоновка рабочего стола)? Тогда окна и док рисуются
// полупрозрачными. При точечных перерисовках стекло кладётся плотно: иначе
// одни и те же пиксели смешивались бы многократно и картинка «плыла».
bool comp_full = false;

// Залить прямоугольник (со скруглением углов r) ЦВЕТНЫМ СТЕКЛОМ.
// alpha = 0..256 — насколько плотно цвет ложится ПОВЕРХ того, что под ним
// (256 = совсем не прозрачно). Смешивание настоящее: читаем пиксель из
// видеопамяти, считаем новое значение — поэтому сквозь окно и док ВИДНО,
// что находится за ними. Так работает «жидкое стекло» без float.
void fill_glass(int x, int y, int w, int h, int r, unsigned int color, int alpha) {
    if (w <= 0 || h <= 0) return;
    int a = comp_full ? alpha : 256;              // вне компоновки — плотно
    if (a < 0) a = 0;
    if (a > 256) a = 256;
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;   // обрезаем по экрану ОДИН раз
    int x1 = x + w,         y1 = y + h;
    if (x1 > W) x1 = W;
    if (y1 > H) y1 = H;
    if (x0 >= x1 || y0 >= y1) return;

    unsigned int ca = encode_color(color);        // цвет сразу в формате экрана
    int ia = 256 - a;
    if (FB_BPP == 32 && FB_GPOS == 8) {
        // --- быстрый путь. Хитрость: байты 0 и 2 (красный и синий) смешиваются
        // ОДНОЙ парой умножений, зелёный (байт 1) — второй. Каждая пара живёт в
        // своих 16 битах (максимум 255*256 = 0xFF00) и соседям не мешает.
        unsigned int cs_rb = ca & 0x00FF00FF;
        unsigned int cs_g  = ca & 0x0000FF00;
        for (int py = y0; py < y1; py++) {
            int j = py - y;
            unsigned int* row = (unsigned int*)(fb + py * FB_PITCH);
            for (int px = x0; px < x1; px++) {
                int i = px - x;
                if (r > 0 && (i < r || i >= w - r) && (j < r || j >= h - r)) {
                    int cx = (i < r) ? r : w - 1 - r;          // центр дуги угла
                    int cy = (j < r) ? r : h - 1 - r;
                    int ex = i - cx, ey = j - cy;
                    if (ex * ex + ey * ey > r * r) continue;   // вне дуги
                }
                unsigned int old = row[px];
                row[px] = ((((old & 0x00FF00FF) * a + cs_rb * ia) >> 8) & 0x00FF00FF)
                        | ((((old & 0x0000FF00) * a + cs_g  * ia) >> 8) & 0x0000FF00);
            }
        }
        return;
    }
    // --- общий путь (24 бита и необычные форматы): по каналам ---
    int rp = FB_RPOS, gp = FB_GPOS, bp = FB_BPOS, n = FB_BPP / 8;
    int cr = (ca >> rp) & 0xFF, cg = (ca >> gp) & 0xFF, cb = (ca >> bp) & 0xFF;
    for (int py = y0; py < y1; py++) {
        int j = py - y;
        unsigned char* row = fb + py * FB_PITCH;
        for (int px = x0; px < x1; px++) {
            int i = px - x;
            if (r > 0 && (i < r || i >= w - r) && (j < r || j >= h - r)) {
                int cx = (i < r) ? r : w - 1 - r;
                int cy = (j < r) ? r : h - 1 - r;
                int ex = i - cx, ey = j - cy;
                if (ex * ex + ey * ey > r * r) continue;
            }
            unsigned char* p = row + px * n;
            unsigned int old = (unsigned int)p[0] | ((unsigned int)p[1] << 8) | ((unsigned int)p[2] << 16);
            int orr = (old >> rp) & 0xFF, og = (old >> gp) & 0xFF, ob = (old >> bp) & 0xFF;
            unsigned int nv = (unsigned int)(((cr * a + orr * ia) >> 8) << rp)
                            | (unsigned int)(((cg * a + og * ia) >> 8) << gp)
                            | (unsigned int)(((cb * a + ob * ia) >> 8) << bp);
            p[0] = (unsigned char)(nv & 0xFF);
            p[1] = (unsigned char)((nv >> 8) & 0xFF);
            p[2] = (unsigned char)((nv >> 16) & 0xFF);
        }
    }
}

// Полупрозрачная точечная заливка поверх УЖЕ нарисованного (фон не трогаем).
// Для тени и разделителя: dens/16 пикселей -> color, остальные остаются.
void dither_over(int x, int y, int w, int h, unsigned int color, int dens) {
    for (int j = y; j < y + h; j++)
        for (int i = x; i < x + w; i++)
            if (i >= 0 && i < W && j >= 0 && j < H)
                if (bayer4[(j & 3) * 4 + (i & 3)] < dens)
                    put_pixel(i, j, color);
}

// Контур скруглённого прямоугольника (тонкая рамка в 1 пиксель)
void draw_round_rect(int x, int y, int w, int h, int r, unsigned int color) {
    for (int i = x + r; i <= x + w - 1 - r; i++) {        // верх и низ (между углами)
        put_pixel(i, y, color);
        put_pixel(i, y + h - 1, color);
    }
    for (int j = y + r; j <= y + h - 1 - r; j++) {        // лево и право
        put_pixel(x, j, color);
        put_pixel(x + w - 1, j, color);
    }
    // угловые дуги (четверти окружности радиуса r)
    for (int dy = -r; dy <= 0; dy++) for (int dx = -r; dx <= 0; dx++)   // лево-верх
        if (dx*dx + dy*dy >= r*r - r && dx*dx + dy*dy <= r*r + r) put_pixel(x + r + dx, y + r + dy, color);
    for (int dy = -r; dy <= 0; dy++) for (int dx = 0; dx <= r; dx++)     // право-верх
        if (dx*dx + dy*dy >= r*r - r && dx*dx + dy*dy <= r*r + r) put_pixel(x + w - 1 - r + dx, y + r + dy, color);
    for (int dy = 0; dy <= r; dy++) for (int dx = -r; dx <= 0; dx++)     // лево-низ
        if (dx*dx + dy*dy >= r*r - r && dx*dx + dy*dy <= r*r + r) put_pixel(x + r + dx, y + h - 1 - r + dy, color);
    for (int dy = 0; dy <= r; dy++) for (int dx = 0; dx <= r; dx++)      // право-низ
        if (dx*dx + dy*dy >= r*r - r && dx*dx + dy*dy <= r*r + r) put_pixel(x + w - 1 - r + dx, y + h - 1 - r + dy, color);
}

// Залить весь экран свежими обоями (рабочий стол)
void fill_wallpaper() {
    for (int y = 0; y < H; y++) fill_rect(0, y, W, 1, wallpaper(y));
}

// ===========================================================================
//  ВИДЕО: «записка» от stage2 (режим VESA VBE поставлен в реальном режиме)
// ===========================================================================
//  stage2 оставляет параметры режима в низкой памяти:
//    0x0504 — dword: адрес видеопамяти (linear framebuffer)
//    0x0508 — word:  байт в строке (pitch)
//    0x050A — word:  ширина
//    0x050C — word:  высота
//    0x050E — byte:  бит на пиксель (24 или 32)
//    0x0510 — word:  сигнатура 'VM' (0x4D56) — «записка настоящая»
//    0x0512 — byte:  позиция битов красного (0..31)
//    0x0513 — byte:  позиция битов зелёного
//    0x0514 — byte:  позиция битов синего
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
            FB_RPOS = rp; FB_GPOS = gp; FB_BPOS = bp;   // порядок каналов от VBE
        }
    }
}

// --- Геометрия окна консоли (плавающее поверх обоев) ---
// Значения считает init_layout() — окно растягивается под ЛЮБОЙ экран,
// который дал VESA (640x480, 1024x768, 1920x1080...): консоль занимает
// почти весь экран, поля по 18 пикселей слева/справа и 4 снизу.
int WIN_X, WIN_Y, WIN_W, WIN_H;             // окно консоли
const int WIN_R = 8;                        // радиус скругления углов
const int TITLE_H = 18;                     // высота шапки окна
int TXT_X, TXT_Y, TXT_W, TXT_H;             // текстовая область внутри окна
int COLS, ROWS;                             // её размер в колонках и строках

// Теневая копия текста консоли (чтобы окно можно было перерисовать, когда его
// перекрыли другие окна). Размер — с запасом под 1024x768 и чуть больше.
const int CG_ROWS = 64, CG_COLS = 200;

// Посчитать геометрию под ТЕКУЩИЙ экран (вызывается после init_video).
// Меню-бар занимает верхние 16 пикселей — окно начинается под ним.
// На 320x200 получились бы прежние числа: окно 18,22,284x174 (9 строк).
void init_layout() {
    WIN_X = 18;
    WIN_Y = 16 + 6;                         // меню-бар + отступ
    WIN_W = W - 2 * WIN_X;
    int bottom = (H >= 400) ? 90 : 4;       // место под док снизу
    WIN_H = H - WIN_Y - bottom;
    TXT_X = WIN_X + 6;                      // левый верх текстовой области
    TXT_Y = WIN_Y + TITLE_H + 6;
    TXT_W = WIN_W - 12;
    TXT_H = WIN_H - TITLE_H - 12;
    COLS = TXT_W / 8;                       // колонок (знакоместо 8 пикселей)
    ROWS = TXT_H / 16;                      // строк (высота строки 16)
    if (COLS > CG_COLS) COLS = CG_COLS;     // но не больше теневой копии текста
    if (ROWS > CG_ROWS) ROWS = CG_ROWS;
}
// Плотность «стекла» (из 256): чем больше, тем менее прозрачно.
// Тело окна почти непрозрачное — так текст читается идеально чётко,
// а шапка, док и иконки «стеклянные» заметнее (сквозь них видно фон).
const int GLASS       = 226;    // тело окна
const int GLASS_TITLE = 150;    // шапка окна
const int GLASS_DOCK  = 145;    // плашка дока
const int GLASS_ICON  = 130;    // подложка иконки
const unsigned int TEXT_COL = C_WHITE;      // белый текст

// Нарисовать плавающее окно в точке (x, y): стеклянное тело, шапка, рамка.
// Общая функция для всех окон (консоль, About, приложения) — геометрию
// задаёт вызывающий, а размеры окна он же кладёт в win_wp/win_hp.
void draw_window_frame(int x, int y, int w, int h, const char* title) {
    // 1. тело окна — почти чёрное стекло (сквозь него чуть видно фон)
    fill_glass(x, y, w, h, WIN_R, C_BLACK, GLASS);
    // 2. шапка — светлее и полупрозрачнее тела: «матовое стекло»
    fill_glass(x + WIN_R / 2, y + 1, w - WIN_R, TITLE_H - 1, 0, C_GLASS_TITLE, GLASS_TITLE);
    // 3. белая рамка в 1 пиксель по контуру
    draw_round_rect(x, y, w, h, WIN_R, C_WHITE);
    // 4. разделитель шапки (тонкая светлая линия)
    dither_over(x + WIN_R, y + TITLE_H, w - 2 * WIN_R, 1, C_WHITE, 7);
    // 5. «светофор» — кнопки окна (красная / жёлтая / зелёная)
    fill_circle(x + 11, y + 9, 3, C_RED);      // красная (закрыть)
    fill_circle(x + 23, y + 9, 3, C_YELLOW);   // жёлтая
    fill_circle(x + 35, y + 9, 3, C_GREEN);    // зелёная
    // 6. заголовок окна (по центру шапки по вертикали)
    draw_text(x + 44, y + 1, title, TEXT_COL);
}

// Окно консоли — просто частный случай общей функции
void draw_console_window() {
    draw_window_frame(WIN_X, WIN_Y, WIN_W, WIN_H, "MyOS CMD");
}

// ===========================================================================
//  ПРОТОТИПЫ (объявления функций, вызываемых раньше их определения)
// ===========================================================================
void run_command(const char* line);
void draw_desktop();
void draw_menubar();
int  boot_partition(unsigned short* out_lba);
void game_open();                        // мини-игра «Змейка»
void code_open();                        // редактор кода MyC
bool user_start(const unsigned char* img, unsigned int len);   // программа в ring 3
void win_place(int a, int w, int h);     // оконный менеджер: поставить окно
void win_raise(int a);                   // поднять окно наверх (и дать фокус)
void dock_launch(int i);                 // запуск/подъём приложения из дока
void inst_open();                        // графический мастер установки (setup)
int  win_w(int want);                    // размер окна — не больше экрана
int  win_h(int want);

// ===========================================================================
//  ОКОННЫЙ МЕНЕДЖЕР — несколько окон на экране одновременно
// ===========================================================================
//  У каждого приложения есть окно со своим положением и «слоем» (z-порядком).
//  Номера приложений совпадают с порядком иконок в доке.
// APP_INSTALL — графический мастер установки: иконки в доке у него нет
// (он вызывается командой setup в консоли), но живёт он по тем же законам:
// своё окно, свой слой, свой фокус клавиатуры.
enum { APP_CMD = 0, APP_FILES, APP_SET, APP_TASKS, APP_CODE, APP_GAME, APP_ABOUT, APP_INSTALL, APP_N };

int  win_x[APP_N], win_y[APP_N];       // положение окна на экране
int  win_wp[APP_N], win_hp[APP_N];     // размер окна (нужен для попадания мышью)
int  win_z[APP_N];                     // слой: больше — выше
int  win_ztop = 0;                     // верхний занятый слой
int  win_focus = -1;                   // у какого окна клавиатура (-1 — рабочий стол)
int  win_casc = 0;                     // счётчик «лестницы» для новых окон

// Поднять окно наверх и отдать ему клавиатуру
void win_raise(int a) {
    if (a < 0 || a >= APP_N) return;
    win_z[a] = ++win_ztop;
    win_focus = a;
}

// Поставить новое окно «лестницей» от центра, не вылезая за экран
void win_place(int a, int w, int h) {
    win_wp[a] = w; win_hp[a] = h;
    int st = win_casc % 6; win_casc++;
    int x = (W - w) / 2 - 110 + st * 46;
    int y = 30 + st * 38;
    int top = 22, bot = 70;              // меню-бар+отступ / место под док
    if (x < 4) x = 4;
    if (x + w > W - 4) x = W - 4 - w;
    if (x < 4) x = 4;
    if (y < top) y = top;
    if (y + h > H - bot) y = H - bot - h;
    if (y < top) y = top;
    win_x[a] = x; win_y[a] = y;
}

// Закрыть окно: фокус уходит верхнему из оставшихся (win_z>0 — «открыто»)
void win_close_app(int a) {
    if (a < 0 || a >= APP_N) return;
    win_z[a] = 0;
    int best = -1, bz = 0;
    for (int i = 0; i < APP_N; i++)
        if (i != a && win_z[i] > bz) { bz = win_z[i]; best = i; }
    win_focus = best;
}

// Положение консоли: её внутренние координаты TXT_* зависят от WIN_*
void console_set_pos(int x, int y) {
    WIN_X = x; WIN_Y = y;
    TXT_X = WIN_X + 6;
    TXT_Y = WIN_Y + TITLE_H + 6;
}

// ===========================================================================
//  КЛАВИАТУРА (опрос портов 0x64/0x60, набор сканкодов Set 1)
// ===========================================================================
//  Свой обработчик прерываний (IDT/IRQ) пока не делаем — вместо этого просто
//  ОПРАШИВАЕМ контроллер клавиатуры: порт 0x64 бит0 = «есть данные»,
//  порт 0x60 = сам сканкод. Работает и без прерываний.
const int SC_F3 = 0x3D;                  // сканкод F3 (нажатие)

// Таблица: сканкод -> символ (строчные/цифры/знаки). 0 = спецклавиша.
unsigned char sc_ascii[0x3A] = {
    // 0x00..0x0F
    0, 0,'1','2','3','4','5','6','7','8','9','0','-', '=', 0, 0,
    // 0x10..0x1F
   'q','w','e','r','t','y','u','i','o','p','[',']', 0, 0,'a','s',
    // 0x20..0x2F
   'd','f','g','h','j','k','l',';','\'','`', 0,'\\','z','x','c','v',
    // 0x30..0x39
   'b','n','m',',','.','/', 0,'*', 0,' '
};

bool shift_down = false;                 // зажат ли Shift

// Прочитать сканкод; -1 = данных нет
int poll_key() {
    unsigned char st = inb(0x64);
    if (!(st & 0x01)) return -1;         // выходной буфер пуст
    if (st & 0x20) return -1;            // байт пришёл от МЫШИ — не наш
    return (int)inb(0x60);
}

// Применить Shift: буква -> ЗАГЛАВНАЯ, цифра/знак -> спецсимвол
char apply_shift(char c) {
    if (c >= 'a' && c <= 'z') return c - 32;
    switch (c) {
        case '1': return '!'; case '2': return '@'; case '3': return '#';
        case '4': return '$'; case '5': return '%'; case '6': return '^';
        case '7': return '&'; case '8': return '*'; case '9': return '(';
        case '0': return ')'; case '-': return '_'; case '=': return '+';
        case '[': return '{'; case ']': return '}'; case ';': return ':';
        case '\'': return '"'; case '`': return '~'; case '\\': return '|';
        case ',': return '<'; case '.': return '>'; case '/': return '?';
    }
    return c;
}

// ===========================================================================
//  МЫШЬ (PS/2) — опрашиваем её сами, как и клавиатуру (без прерываний)
// ===========================================================================
//  1) разрешаем «вспомогательный порт» (0xA8) и открываем тактовый генератор
//     мыши в байте конфигурации (порты 0x64/0x60);
//  2) шлём команды мыши через префикс 0xD4 (0xF6 — умолчания, 0xF4 — начать
//     передачу данных);
//  3) в главном цикле вычитываем 3-байтные пакеты и двигаем курсор.
int mouse_x = 160, mouse_y = 100;        // позиция стрелки (обновит init_desktop_layout)
int mouse_ok = 0;                        // 1 — мышь нашлась и настроена
int mouse_ack = -1;                      // ответ мыши на 0xF4 (диагностика)
int mouse_btn = 0;                       // 1 — левая, 2 — правая, 4 — средняя

void ps2_wr_wait() { for (int i = 0; i < 100000; i++) if (!(inb(0x64) & 0x02)) return; }
void ps2_rd_wait() { for (int i = 0; i < 100000; i++) if (inb(0x64) & 0x01) return; }

// Команда мыши: префикс 0xD4 («вспомогательное устройство»), затем байт.
// Возврат — ответ мыши (успех: 0xFA, ACK).
int mouse_cmd(unsigned char v) {
    ps2_wr_wait(); outb(0x64, 0xD4);
    ps2_wr_wait(); outb(0x60, v);
    ps2_rd_wait(); return inb(0x60);
}

void mouse_init() {
    ps2_wr_wait(); outb(0x64, 0xA8);         // включить вспомогательное устройство
    ps2_wr_wait(); outb(0x64, 0x20);         // прочитать байт конфигурации
    ps2_rd_wait(); unsigned char cfg = inb(0x60);
    cfg = (unsigned char)(cfg & ~0x20);      // открыть тактовый генератор мыши
    cfg = (unsigned char)(cfg & ~0x02);      // IRQ12 не нужен — опрашиваем сами
    ps2_wr_wait(); outb(0x64, 0x60);
    ps2_wr_wait(); outb(0x60, cfg);
    mouse_cmd(0xF6);                         // настройки по умолчанию
    int ack = mouse_cmd(0xF4);               // включить передачу данных
    mouse_ack = ack;
    mouse_ok = (ack == 0xFA);                // мыши нет — курсор не показываем
}

// Вычитать все готовые пакеты (по 3 байта). Возврат: 1 — координаты обновились
int mouse_read() {
    static unsigned char pk[3]; static int pi = 0;
    int got = 0;
    while ((inb(0x64) & 0x21) == 0x21) {     // в буфере байт ИМЕННО от мыши
        unsigned char b = inb(0x60);
        if (pi == 0 && !(b & 0x08)) continue;   // бит-сигнатура; иначе ресинхронизация
        pk[pi++] = b;
        if (pi == 3) {
            pi = 0;
            int dx = pk[1], dy = pk[2];
            if (pk[0] & 0x10) dx -= 256;     // знак X
            if (pk[0] & 0x20) dy -= 256;     // знак Y
            mouse_x += dx;
            mouse_y -= dy;                   // экранный Y растёт вниз, у мыши — вверх
            mouse_btn = pk[0] & 0x07;
            if (mouse_x < 0) mouse_x = 0;
            if (mouse_y < 0) mouse_y = 0;
            if (mouse_x > W - 1) mouse_x = W - 1;
            if (mouse_y > H - 1) mouse_y = H - 1;
            got = 1;
        }
    }
    return got;
}

// --- Курсор-стрелка -------------------------------------------------------
//  Перед любой перерисовкой курсор ПРЯЧЕМ (возвращаем фон, который был под
//  стрелкой), после — показываем заново. Так стрелку не «размазываем» и не
//  перерисовываем весь экран из-за каждого движения мыши.
//  Форма: голова-треугольник (0,0)-(9,9)-(0,13) и «ножка» вниз-вправо.
//  Контур чёрный, заливка белая — стрелку видно и на светлом, и на тёмном.
int cur_solid(int x, int y) {
    if (x < 0 || y < 0) return 0;
    if (y <= 9 && x <= y) return 1;                            // голова растёт
    if (y >= 10 && y <= 13 && x <= 9 - (y - 9) * 2) return 1;  // ...и сужается книзу
    if (y >= 9 && y <= 17) { int a = y - 3; if (x >= a && x <= a + 2) return 1; }  // ножка
    return 0;
}
int cur_edge(int x, int y) {                 // пиксель контура (сосед заливки)
    if (cur_solid(x, y)) return 0;
    return cur_solid(x - 1, y) || cur_solid(x + 1, y) ||
           cur_solid(x, y - 1) || cur_solid(x, y + 1);
}

const int CUR_BW = 20, CUR_BH = 20;                    // габарит с запасом на контур
unsigned char cur_bg[CUR_BW * CUR_BH * 4];             // «что было под стрелкой»
int cur_bx = 0, cur_by = 0, cur_on = 0;

void cursor_hide() {
    if (!cur_on) return;
    int n = FB_BPP / 8;
    for (int j = 0; j < CUR_BH; j++) {
        int y = cur_by + j; if (y < 0 || y >= H) continue;
        for (int i = 0; i < CUR_BW; i++) {
            int x = cur_bx + i; if (x < 0 || x >= W) continue;
            unsigned char* p = fb + y * FB_PITCH + x * n;
            unsigned char* s = cur_bg + (j * CUR_BW + i) * n;
            for (int k = 0; k < n; k++) p[k] = s[k];
        }
    }
    cur_on = 0;
}

void cursor_show() {
    if (!mouse_ok) return;
    if (cur_on) cursor_hide();                    // защита от двойного показа
    int n = FB_BPP / 8;
    cur_bx = mouse_x - 1; cur_by = mouse_y - 1;   // 1 пиксель — под контур
    for (int j = 0; j < CUR_BH; j++) {
        int y = cur_by + j; if (y < 0 || y >= H) continue;
        for (int i = 0; i < CUR_BW; i++) {
            int x = cur_bx + i; if (x < 0 || x >= W) continue;
            unsigned char* p = fb + y * FB_PITCH + x * n;
            unsigned char* s = cur_bg + (j * CUR_BW + i) * n;
            for (int k = 0; k < n; k++) s[k] = p[k];       // запомнить фон
            int lx = x - mouse_x, ly = y - mouse_y;
            if (cur_solid(lx, ly)) put_pixel(x, y, C_WHITE);
            else if (cur_edge(lx, ly)) put_pixel(x, y, C_BLACK);
        }
    }
    cur_on = 1;
}

// ===========================================================================
//  КОНСОЛЬ (CMD): курсор, вывод, ввод — всё внутри стеклянного окна
// ===========================================================================
// Теневая копия текста консоли: экран можно перерисовать (окно перекрыли
// другие окна), не потеряв содержимое. Обновляется в cmd_putchar/redraw_input.
char cmd_grid[CG_ROWS][CG_COLS];
int  cur_col = 0, cur_row = 0;           // позиция курсора (колонка/строка)
char cmd_line[161];                      // буфер вводимой команды (до 160 знаков)
int  cmd_len = 0;
bool cmd_active = false;                 // открыта ли консоль
bool e0_prefix = false;                  // был префикс E0 (стрелки и др. клавиши)
char hist[8][161];                       // история команд (до 8)
int  hist_count = 0;                     // сколько команд сохранено
int  hist_pos = 0;                       // позиция (hist_count = свежая строка)

// Сдвинуть текстовую область вверх на одну строку шрифта (16 пикселей).
// Копируем ПОБАЙТОВО строки друг на друга с учётом pitch видеопамяти
// (в 24/32-битных режимах пиксель занимает более одного байта!).
void scroll_up() {
    int px = FB_BPP / 8;                 // байт на пиксель (3 или 4)
    int rowbytes = TXT_W * px;           // байт в одной строке текста
    unsigned char* dst = fb + TXT_Y * FB_PITCH + TXT_X * px;
    unsigned char* src = fb + (TXT_Y + 16) * FB_PITCH + TXT_X * px;
    for (int j = 0; j < TXT_H - 16; j++) {
        for (int i = 0; i < rowbytes; i++) dst[i] = src[i];
        dst += FB_PITCH;
        src += FB_PITCH;
    }
    // освободившуюся снизу строку — заново «стеклом» (просвет = свежие обои)
    fill_rect(TXT_X, TXT_Y + TXT_H - 16, TXT_W, 16, C_BLACK);
}

void cmd_newline() {
    cur_col = 0;
    cur_row++;
    if (cur_row >= ROWS) {                       // экран кончился — прокрутка
        scroll_up();
        cur_row = ROWS - 1;
        for (int j = 0; j + 1 < ROWS && j + 1 < CG_ROWS; j++)     // и теневую копию
            for (int i = 0; i < COLS && i < CG_COLS; i++) cmd_grid[j][i] = cmd_grid[j + 1][i];
        for (int i = 0; i < COLS && i < CG_COLS; i++) cmd_grid[cur_row][i] = ' ';
    }
}

// Нарисовать символ в позиции курсора (внутри окна) и сдвинуть курсор
void cmd_putchar(char c) {
    if (c == '\n') { cmd_newline(); return; }
    if (c == '\r') return;
    if (cur_row < CG_ROWS && cur_col < CG_COLS) cmd_grid[cur_row][cur_col] = c;
    draw_char(TXT_X + cur_col * 8, TXT_Y + cur_row * 16, c, TEXT_COL);
    cur_col++;
    if (cur_col >= COLS) cmd_newline();
}

void cmd_print(const char* s) { while (*s) cmd_putchar(*s++); }

// Нарисовать окно консоли заново по теневой копии текста
void console_repaint() {
    draw_console_window();
    for (int r = 0; r < ROWS && r < CG_ROWS; r++) {
        int y = TXT_Y + r * 16;
        for (int i = 0; i < COLS && i < CG_COLS; i++) {
            char c = cmd_grid[r][i];
            if (c && c != ' ') draw_char(TXT_X + i * 8, y, c, TEXT_COL);
        }
    }
}

// Стереть символ слева от курсора (Backspace) — вернуть «стекло» на место
void cmd_backspace() {
    if (cmd_len == 0) return;
    cmd_len--;
    if (cur_col > 0) {
        cur_col--;
        if (cur_row < CG_ROWS && cur_col < CG_COLS) cmd_grid[cur_row][cur_col] = ' ';
        fill_rect(TXT_X + cur_col * 8, TXT_Y + cur_row * 16, 8, 16, C_BLACK);
    }
}

// Открыть консоль: поставить окно «лестницей», поднять, очистить текст
void cmd_open() {
    win_place(APP_CMD, WIN_W, WIN_H);
    win_raise(APP_CMD);
    for (int r = 0; r < CG_ROWS; r++)         // чистая теневая копия текста
        for (int i = 0; i < CG_COLS; i++) cmd_grid[r][i] = 0;
    cur_col = 0; cur_row = 0; cmd_len = 0;
    cmd_active = true;
    draw_desktop();                           // окно + другие окна + рабочий стол
    cmd_print("MyOS CMD v0.1\n");
    cmd_print("Type 'help'. F3 closes.\n");
    cmd_print("Setup OS: 'setup'\n");
    cmd_print("> ");
}

void cmd_submit() {
    cmd_line[cmd_len] = 0;               // завершить C-строку
    // запомнить непустую команду в истории (для стрелок Up/Down)
    if (cmd_len > 0) {
        if (hist_count == 8) {            // место кончилось — сдвигаем, старая отпадает
            for (int i = 0; i < 7; i++) {
                int k = 0; while (hist[i + 1][k]) { hist[i][k] = hist[i + 1][k]; k++; }
                hist[i][k] = 0;
            }
            hist_count = 7;
        }
        int k = 0; while (cmd_line[k]) { hist[hist_count][k] = cmd_line[k]; k++; }
        hist[hist_count][k] = 0;
        hist_count++;
        hist_pos = hist_count;            // «свежая» строка (ничего не recalled)
    }
    cmd_putchar('\n');
    run_command(cmd_line);
    cmd_len = 0;
    if (!cmd_active) return;             // команда открыла приложение (play/code) — консоль закрыта
    if (cur_col != 0) cmd_putchar('\n'); // промпт всегда на новой строке
    cmd_print("> ");
}

// --- История команд (стрелки Up / Down) ---

// Перерисовать строку ввода: очистить вводимую часть и написать cmd_line заново
void redraw_input() {
    fill_rect(TXT_X + 2 * 8, TXT_Y + cur_row * 16, (COLS - 2) * 8, 16, C_BLACK);
    if (cur_row < CG_ROWS)                                   // чистим и теневую копию
        for (int i = 2; i < COLS && i < CG_COLS; i++) cmd_grid[cur_row][i] = ' ';
    int cc = 2;                          // ввод идёт после промпта "> "
    for (int i = 0; i < cmd_len; i++) {
        if (cur_row < CG_ROWS && cc < CG_COLS) cmd_grid[cur_row][cc] = cmd_line[i];
        draw_char(TXT_X + cc * 8, TXT_Y + cur_row * 16, cmd_line[i], TEXT_COL);
        cc++;
    }
    cur_col = 2 + cmd_len;
}

// Загрузить команду из истории в строку ввода
void hist_load(const char* src) {
    cmd_len = 0;
    while (src[cmd_len] && cmd_len < COLS - 3 && cmd_len < 160) { cmd_line[cmd_len] = src[cmd_len]; cmd_len++; }
    redraw_input();
}

// Up — предыдущая команда
void hist_up() {
    if (hist_count == 0) return;
    if (hist_pos > 0) hist_pos--;
    hist_load(hist[hist_pos]);
}

// Down — следующая команда (в конце — очистить строку до свежей)
void hist_down() {
    if (hist_pos < hist_count) hist_pos++;
    if (hist_pos >= hist_count) { cmd_len = 0; redraw_input(); }
    else hist_load(hist[hist_pos]);
}

// ===========================================================================
//  ОБРАБОТКА НАЖАТИЙ В КОНСОЛИ
// ===========================================================================
void cmd_handle_key(int sc) {
    if (sc == 0xE0) { e0_prefix = true; return; }  // ждём 2-й байт (стрелки и др.)
    bool ext = e0_prefix; e0_prefix = false;

    if (sc == SC_F3 && !ext) {           // F3 — закрыть консоль
        cmd_active = false;
        win_close_app(APP_CMD);          // вернуть фокус другому окну
        draw_desktop();                  // перерисовать рабочий стол
        return;
    }
    if (sc == 0x01 && !ext) return;      // Esc в консоли ничего не делает
    if (ext) {                           // расширенные клавиши (после E0)
        if (!(sc & 0x80)) {              // только нажатия (не отпускания)
            if (sc == 0x48) hist_up();       // Up   — предыдущая команда
            if (sc == 0x50) hist_down();     // Down — следующая команда
        }
        return;
    }
    if (sc & 0x80) {                     // отпускание клавиши (break-код)
        if ((sc & 0x7F) == 0x2A || (sc & 0x7F) == 0x36) shift_down = false;
        return;
    }
    // нажатие клавиши (make-код)
    if (sc == 0x2A || sc == 0x36) { shift_down = true; return; }   // Shift
    if (sc == 0x1C) { cmd_submit(); return; }                      // Enter
    if (sc == 0x0E) { cmd_backspace(); return; }                   // Backspace

    if (sc < 0x3A) {                     // обычный символ
        char c = (char)sc_ascii[sc];
        if (c) {
            if (shift_down) c = apply_shift(c);
            if (cmd_len < COLS - 3 && cmd_len < 160) {   // строка влезает в буфер
                cmd_line[cmd_len++] = c;
                cmd_putchar(c);
            }
        }
    }
}

// ===========================================================================
//  СТРОКОВЫЕ УТИЛИТЫ И КОМАНДЫ
// ===========================================================================
bool str_eq(const char* a, const char* b) {
    while (*a && *b) { if (*a != *b) return false; a++; b++; }
    return *a == *b;
}
bool str_startswith(const char* s, const char* p) {
    while (*p) { if (*s != *p) return false; s++; p++; }
    return true;
}

// --- Печать чисел и чтение часов CMOS ---
// Напечатать двузначное число (без лишних нулей)
void print2(int n) {
    if (n >= 10) cmd_putchar('0' + n / 10);   // десятки
    cmd_putchar('0' + n % 10);                // единицы
}

// Прочитать регистр часов реального времени CMOS (порты 0x70/0x71)
unsigned char cmos_read(unsigned char reg) {
    outb(0x70, reg);                          // выбрать регистр
    return inb(0x71);                         // прочитать значение
}
int bcd2bin(unsigned char b) { return (b & 0x0F) + ((b >> 4) * 10); } // BCD -> число

// ===========================================================================
//  ДИСК: чтение секторов по ATA PIO (первичный канал, master)
// ===========================================================================
//  В защищённом режиме BIOS (int 0x13) недоступен, поэтому говорим с диском
//  напрямую через порты ATA-контроллера 0x1F0..0x1F7.
//  PIO = программный ввод-вывод: выбираем сектор, даём команду, ждём готовности
//  и читаем 256 слов (512 байт = ровно один сектор).
const unsigned short ATA_IO   = 0x1F0;   // данные (чтение — по 16 бит)
const unsigned short ATA_CNT  = 0x1F2;   // сколько секторов читать
const unsigned short ATA_LBA0 = 0x1F3;   // LBA, байт 0
const unsigned short ATA_LBA1 = 0x1F4;   // LBA, байт 1
const unsigned short ATA_LBA2 = 0x1F5;   // LBA, байт 2
const unsigned short ATA_DRV  = 0x1F6;   // выбор диска + старшие биты LBA
const unsigned short ATA_CMD  = 0x1F7;   // команда (запись) / статус (чтение)
const unsigned short ATA_ALT  = 0x3F6;   // альтернативный статус (для задержки)
const unsigned int DISK_SECTORS = 65536; // размер нашего образа: 32 МБ = 65536 секторов

// Сколько секторов занимает само ядро на диске. Число ВШИВАЕТСЯ при сборке
// (build.ps1 компилирует дважды и подставляет -DKERNEL_SECTORS=...).
// Нужно команде install: она копирует ядро из начала диска в раздел.
#ifndef KERNEL_SECTORS
#define KERNEL_SECTORS 48      // запасной вариант, если собрали без параметра
#endif

// Ждать, пока диск не «занят» (BSY = 0)
void ata_wait_bsy() {
    for (int t = 0; t < 1000000; t++)
        if (!(inb(ATA_CMD) & 0x80)) return;   // бит 7 = BSY (занят)
}
// Ждать, пока появились данные (DRQ = 1)
void ata_wait_drq() {
    for (int t = 0; t < 1000000; t++)
        if (inb(ATA_CMD) & 0x08) return;      // бит 3 = DRQ (данные готовы)
}
// Маленькая пауза: ATA хочет ~400 нс между записью регистров.
// Чтение альтернативного статуса занимает ~100 нс, читаем 4 раза.
void ata_delay() {
    for (int i = 0; i < 4; i++) inb(ATA_ALT);
}

// Прочитать ОДИН сектор (512 байт) по 28-битному адресу LBA в буфер
void ata_read_sector(unsigned int lba, unsigned char* buf) {
    ata_wait_bsy();
    outb(ATA_DRV, 0xE0 | ((lba >> 24) & 0x0F)); // master + старшие 4 бита LBA
    ata_delay();
    outb(ATA_CNT,  1);                          // читаем ровно 1 сектор
    outb(ATA_LBA0, (unsigned char)(lba & 0xFF));
    outb(ATA_LBA1, (unsigned char)((lba >> 8) & 0xFF));
    outb(ATA_LBA2, (unsigned char)((lba >> 16) & 0xFF));
    outb(ATA_CMD,  0x20);                       // команда READ SECTORS
    ata_wait_bsy();
    ata_wait_drq();
    unsigned short* w = (unsigned short*)buf;
    for (int i = 0; i < 256; i++) w[i] = inw(ATA_IO);  // 256 слов = 512 байт
}

// Записать ОДИН сектор (512 байт) по адресу LBA ИЗ буфера
void ata_write_sector(unsigned int lba, unsigned char* buf) {
    ata_wait_bsy();
    outb(ATA_DRV, 0xE0 | ((lba >> 24) & 0x0F)); // master + старшие 4 бита LBA
    ata_delay();
    outb(ATA_CNT,  1);                          // пишем ровно 1 сектор
    outb(ATA_LBA0, (unsigned char)(lba & 0xFF));
    outb(ATA_LBA1, (unsigned char)((lba >> 8) & 0xFF));
    outb(ATA_LBA2, (unsigned char)((lba >> 16) & 0xFF));
    outb(ATA_CMD,  0x30);                       // команда WRITE SECTORS
    ata_wait_bsy();
    ata_wait_drq();
    unsigned short* w = (unsigned short*)buf;
    for (int i = 0; i < 256; i++) outw(ATA_IO, w[i]);  // 256 слов = 512 байт
    outb(ATA_CMD, 0xE7);                        // FLUSH CACHE: сбросить кэш на диск
    ata_wait_bsy();
}

// Буфер под один сектор диска (глобальный — не держим 512 байт на стеке)
unsigned char disk_buf[512];

// --- Печать чисел для команды part ---
void print_hex8(unsigned char v) {
    const char* h = "0123456789ABCDEF";
    cmd_putchar(h[(v >> 4) & 0x0F]);
    cmd_putchar(h[v & 0x0F]);
}
void print_dec(unsigned int n) {
    if (n == 0) { cmd_putchar('0'); return; }
    char t[12]; int i = 0;
    while (n > 0) { t[i++] = (char)('0' + (n % 10)); n /= 10; }
    while (i > 0) cmd_putchar(t[--i]);
}

// Команда part: читаем сектор 0 (MBR) через ATA и печатаем таблицу разделов
void cmd_part() {
    ata_read_sector(0, disk_buf);
    if (disk_buf[510] != 0x55 || disk_buf[511] != 0xAA) {
        cmd_print("No MBR signature (disk is not ATA?)\n");
        return;
    }
    cmd_print("MBR partitions:\n");
    for (int i = 0; i < 4; i++) {
        unsigned char* e = disk_buf + 446 + i * 16;      // запись раздела i
        if (e[4] == 0) continue;                         // пустая запись — пропуск
        unsigned int lba = (unsigned int)e[8]  | ((unsigned int)e[9]  << 8) |
                           ((unsigned int)e[10] << 16) | ((unsigned int)e[11] << 24);
        unsigned int sec = (unsigned int)e[12] | ((unsigned int)e[13] << 8) |
                           ((unsigned int)e[14] << 16) | ((unsigned int)e[15] << 24);
        cmd_print("  P");
        cmd_putchar((char)('1' + i));
        cmd_print(" type=");
        print_hex8(e[4]);
        cmd_print(" lba=");
        print_dec(lba);
        cmd_print(" size=");
        print_dec(sec / 2048);                           // 2048 секторов = 1 МБ
        cmd_print("MB\n");
    }
}

// Команда wtest: проверяем ЗАПИСЬ на диск.
// Пишем в свободный сектор узор, стираем буфер, читаем обратно и сверяем.
void cmd_wtest() {
    const unsigned int LBA = 60000;              // свободный сектор (после разделов)
    cmd_print("Write test @ LBA ");
    print_dec(LBA);
    cmd_print("...\n");
    // 1. Заполняем буфер узором: байт i = i*7+13 (перестановка байт сразу заметна)
    for (int i = 0; i < 512; i++) disk_buf[i] = (unsigned char)(i * 7 + 13);
    // 2. Пишем сектор на диск (ATA WRITE SECTORS + FLUSH)
    ata_write_sector(LBA, disk_buf);
    // 3. Стираем буфер — чтобы обратное чтение было ЧЕСТНЫМ (не остатки в памяти)
    for (int i = 0; i < 512; i++) disk_buf[i] = 0;
    // 4. Читаем сектор обратно
    ata_read_sector(LBA, disk_buf);
    // 5. Сверяем байт за байтом
    int bad = -1;
    for (int i = 0; i < 512; i++)
        if (disk_buf[i] != (unsigned char)(i * 7 + 13)) { bad = i; break; }
    if (bad < 0) {
        cmd_print("  OK - 512 bytes match\n");
    } else {
        cmd_print("  FAIL at byte ");
        print_dec((unsigned int)bad);
        cmd_print("\n");
    }
}

// Команда mkpart: создать (или изменить) раздел N размером MB мегабайт.
// Пишем ТОЛЬКО байты 446..509 сектора 0 — загрузочный код в начале не трогаем!
void cmd_mkpart(int n, int mb) {
    if (n < 1 || n > 4) {
        cmd_print("Usage: mkpart N MB  (N=1..4)\n");
        return;
    }
    if (mb < 1 || mb > 32) {
        cmd_print("Size must be 1..32 MB\n");
        return;
    }
    ata_read_sector(0, disk_buf);                    // читаем сектор 0 (код + таблица)
    if (disk_buf[510] != 0x55 || disk_buf[511] != 0xAA) {
        cmd_print("No MBR signature\n");
        return;
    }
    unsigned char* e = disk_buf + 446 + (n - 1) * 16;   // запись раздела N
    unsigned int start;
    if (e[4] != 0) {
        // слот уже занят — оставляем его начало, меняем только размер
        start = (unsigned int)e[8]  | ((unsigned int)e[9]  << 8) |
                ((unsigned int)e[10] << 16) | ((unsigned int)e[11] << 24);
    } else {
        // слот пуст — ставим новый раздел после самого дальнего занятого
        start = 2048;                                // минимум: 1 МБ от начала диска
        for (int i = 0; i < 4; i++) {
            unsigned char* o = disk_buf + 446 + i * 16;
            if (o[4] == 0) continue;                 // пустая запись — пропуск
            unsigned int olba = (unsigned int)o[8]  | ((unsigned int)o[9]  << 8) |
                                ((unsigned int)o[10] << 16) | ((unsigned int)o[11] << 24);
            unsigned int osec = (unsigned int)o[12] | ((unsigned int)o[13] << 8) |
                                ((unsigned int)o[14] << 16) | ((unsigned int)o[15] << 24);
            if (olba + osec > start) start = olba + osec;
        }
        start = ((start + 2047) / 2048) * 2048;      // выравниваем начало на 1 МБ
    }
    unsigned int size = (unsigned int)mb * 2048;     // размер: МБ -> сектора
    if (start + size > DISK_SECTORS) {               // не вылезаем за диск
        cmd_print("No space: only ");
        print_dec((DISK_SECTORS - start) / 2048);
        cmd_print("MB free\n");
        return;
    }
    // Заполняем запись раздела (16 байт) — как в настоящем MBR
    if (e[4] == 0) e[0] = 0;                         // новый раздел — не загрузочный
    e[1] = 0; e[2] = 0; e[3] = 0;                    // CHS начала (для LBA не нужен)
    e[4] = 0x83;                                     // тип раздела: MyOS
    e[5] = 0; e[6] = 0; e[7] = 0;                    // CHS конца (для LBA не нужен)
    e[8]  = (unsigned char)(start & 0xFF);           // LBA начала (4 байта)
    e[9]  = (unsigned char)((start >> 8) & 0xFF);
    e[10] = (unsigned char)((start >> 16) & 0xFF);
    e[11] = (unsigned char)((start >> 24) & 0xFF);
    e[12] = (unsigned char)(size & 0xFF);            // размер в секторах (4 байта)
    e[13] = (unsigned char)((size >> 8) & 0xFF);
    e[14] = (unsigned char)((size >> 16) & 0xFF);
    e[15] = (unsigned char)((size >> 24) & 0xFF);
    // Пишем сектор 0 обратно и сразу же показываем таблицу (контрольное чтение)
    ata_write_sector(0, disk_buf);
    cmd_print("Created P");
    cmd_putchar((char)('0' + n));
    cmd_print(": lba=");
    print_dec(start);
    cmd_print(" size=");
    print_dec((unsigned int)mb);
    cmd_print("MB\n");
    cmd_part();
}

// ===========================================================================
//  MyFS — СВОЯ ФАЙЛОВАЯ СИСТЕМА (суперблок + таблица файлов + данные)
// ===========================================================================
//  Раскладка раздела:
//    +0 : суперблок      (magic "MYFS", размеры, где таблица, где данные)
//    +1 : таблица файлов (8 записей по 64 байта = ровно 1 сектор)
//    +2 : область данных (файлы лежат подряд, один за другим)
//  Файлы хранятся ЦЕЛИКОМ подряд — для учебной ОС этого достаточно, а устроено
//  всё как у взрослых ФС: метаданные (суперблок, таблица) отдельно от данных.

struct Superblock {
    char magic[4];                 // 'M','Y','F','S'
    unsigned short version;        // версия формата = 1
    unsigned int total_sectors;    // размер раздела в секторах
    unsigned int table_lba;        // LBA таблицы файлов
    unsigned int data_lba;         // LBA начала области данных
    unsigned int data_ptr;         // первый СВОБОДНЫЙ сектор данных
    unsigned short file_count;     // сколько записей занято
    unsigned short files_max;      // сколько записей всего (8)
} __attribute__((packed));

struct FileEntry {
    char name[56];                 // имя файла (заканчивается нулём)
    unsigned int start;            // LBA начала данных
    unsigned int size;             // размер в байтах
} __attribute__((packed));

const int FS_FILES_MAX = 8;        // 8 записей по 64 байта = ровно 1 сектор
unsigned char sec_buf[512];        // второй буфер для ФС (первый занят MBR)

// Первые PART_RESERVED секторов ЛЮБОГО раздела — служебная зона загрузки
// (её пишет install):
//   +0     VBR — загрузочный сектор РАЗДЕЛА (копия boot.asm с метками раздела)
//   +1..+2 stage2 (2 сектора: VESA + загрузка ядра)
//   +3..   ядро (сколько секторов — знает константа KERNEL_SECTORS)
// MyFS начинается ТОЛЬКО после этой зоны, поэтому format и install не мешают
// друг другу: install перезаписывает зону загрузки, файлы MyFS остаются целы.
const unsigned int PART_RESERVED = 192;  // 192 сектора = 96 КБ (запас для роста ядра)

// Найти раздел N в MBR: вернуть LBA начала и размер (в секторах).
// false = раздела нет. ВНИМАНИЕ: читает сектор 0 в disk_buf и затирает его!
bool find_part(int n, unsigned int* lba, unsigned int* size) {
    if (n < 1 || n > 4) return false;                 // защита от выхода за буфер
    ata_read_sector(0, disk_buf);
    if (disk_buf[510] != 0x55 || disk_buf[511] != 0xAA) return false;
    unsigned char* e = disk_buf + 446 + (n - 1) * 16;
    if (e[4] == 0) return false;                      // слот пуст
    *lba  = (unsigned int)e[8]  | ((unsigned int)e[9]  << 8) |
            ((unsigned int)e[10] << 16) | ((unsigned int)e[11] << 24);
    *size = (unsigned int)e[12] | ((unsigned int)e[13] << 8) |
            ((unsigned int)e[14] << 16) | ((unsigned int)e[15] << 24);
    return true;
}

// В disk_buf сейчас суперблок MyFS?
bool is_myfs() {
    return disk_buf[0] == 'M' && disk_buf[1] == 'Y' &&
           disk_buf[2] == 'F' && disk_buf[3] == 'S';
}

// Форматирование: создать пустую MyFS в разделе N.
// Тихий режим (quiet) — без печати в консоль: так форматирует проводник.
// Возврат: 0 — готово, -1 — нет такого раздела.
int fs_format(int n, bool quiet = false) {
    unsigned int plba, psize;
    if (!find_part(n, &plba, &psize)) {
        if (!quiet) cmd_print("No such partition\n");
        return -1;
    }
    for (int i = 0; i < 512; i++) disk_buf[i] = 0;    // чистим сектор суперблока
    Superblock* sb = (Superblock*)disk_buf;
    sb->magic[0] = 'M'; sb->magic[1] = 'Y'; sb->magic[2] = 'F'; sb->magic[3] = 'S';
    sb->version       = 1;
    sb->total_sectors = psize;
    sb->table_lba     = plba + PART_RESERVED + 1;     // таблица — сразу за суперблоком
    sb->data_lba      = plba + PART_RESERVED + 2;     // а дальше — данные
    sb->data_ptr      = plba + PART_RESERVED + 2;     // данных пока нет
    sb->file_count    = 0;
    sb->files_max     = FS_FILES_MAX;
    ata_write_sector(plba + PART_RESERVED, disk_buf);
    for (int i = 0; i < 512; i++) disk_buf[i] = 0;    // пустая таблица файлов
    ata_write_sector(plba + PART_RESERVED + 1, disk_buf);
    if (!quiet) {
        cmd_print("Formatted P");
        cmd_putchar((char)('0' + n));
        cmd_print(" as MyFS\n");
    }
    return 0;
}

// Список файлов раздела N
void fs_ls(int n) {
    unsigned int plba, psize;
    if (!find_part(n, &plba, &psize)) { cmd_print("No such partition\n"); return; }
    ata_read_sector(plba + PART_RESERVED, disk_buf);
    if (!is_myfs()) { cmd_print("Not MyFS (format it first)\n"); return; }
    Superblock* sb = (Superblock*)disk_buf;
    ata_read_sector(sb->table_lba, sec_buf);
    cmd_print("MyFS on P");
    cmd_putchar((char)('0' + n));
    cmd_print(":\n");
    int shown = 0;
    for (int i = 0; i < FS_FILES_MAX; i++) {
        FileEntry* f = (FileEntry*)(sec_buf + i * 64);
        if (f->name[0] == 0) continue;                // пустая запись
        cmd_print("  ");
        cmd_print(f->name);
        cmd_print("  ");
        print_dec(f->size);
        cmd_print(" bytes\n");
        shown++;
    }
    if (shown == 0) cmd_print("  (empty)\n");
}

// Записать файл name с текстом text в раздел N.
// Возврат: 0 — ок, -1 — нет раздела, -2 — не MyFS, -3 — имя занято,
//          -4 — таблица заполнена, -5 — нет места.
// quiet — без печати в консоль (так пишет проводник).
int fs_write(int n, const char* name, const char* text, bool quiet = false) {
    unsigned int plba, psize;
    if (!find_part(n, &plba, &psize)) {
        if (!quiet) cmd_print("No such partition\n");
        return -1;
    }
    ata_read_sector(plba + PART_RESERVED, disk_buf);
    if (!is_myfs()) {
        if (!quiet) cmd_print("Not MyFS (format it first)\n");
        return -2;
    }
    Superblock* sb = (Superblock*)disk_buf;
    unsigned int table = sb->table_lba;
    unsigned int dptr  = sb->data_ptr;
    // читаем таблицу: ищем свободную запись и проверяем дубликаты имён
    ata_read_sector(table, sec_buf);
    int free_idx = -1;
    for (int i = 0; i < FS_FILES_MAX; i++) {
        FileEntry* f = (FileEntry*)(sec_buf + i * 64);
        if (f->name[0] == 0) { if (free_idx < 0) free_idx = i; continue; }
        if (str_eq(f->name, name)) {
            if (!quiet) cmd_print("File exists\n");
            return -3;
        }
    }
    if (free_idx < 0) {
        if (!quiet) cmd_print("File table full\n");
        return -4;
    }
    int len = 0;
    while (text[len]) len++;                          // длина текста
    unsigned int need = ((unsigned int)len + 511) / 512;   // сколько секторов занять
    if (dptr + need > plba + psize) {
        if (!quiet) cmd_print("No space on partition\n");
        return -5;
    }
    // обновляем суперблок СРАЗУ, пока disk_buf ещё держит его в руках
    sb->data_ptr = dptr + need;
    sb->file_count = (unsigned short)(sb->file_count + 1);
    ata_write_sector(plba + PART_RESERVED, disk_buf);
    // пишем данные: каждые 512 байт текста -> один сектор
    for (unsigned int s = 0; s < need; s++) {
        for (int i = 0; i < 512; i++) {
            int pos = (int)(s * 512) + i;
            disk_buf[i] = (pos < len) ? (unsigned char)text[pos] : 0;
        }
        ata_write_sector(dptr + s, disk_buf);
    }
    // заполняем запись в таблице и пишем её на диск
    FileEntry* f = (FileEntry*)(sec_buf + free_idx * 64);
    for (int i = 0; i < 56; i++) f->name[i] = 0;
    int k = 0;
    while (name[k] && k < 55) { f->name[k] = name[k]; k++; }
    f->start = dptr;
    f->size  = (unsigned int)len;
    ata_write_sector(table, sec_buf);
    if (!quiet) {
        cmd_print("Written "); cmd_print(name);
        cmd_print(", "); print_dec((unsigned int)len); cmd_print(" bytes\n");
    }
    return 0;
}

// Удалить файл name из раздела N. Данные «уплотняются»: файлы, лежавшие
// после удалённого, сдвигаются на его место — освободившееся место можно
// сразу занять новыми файлами.
// Возврат: 0 — ок, -1 — нет раздела, -2 — не MyFS, -3 — нет файла.
int fs_delete(int n, const char* name) {
    unsigned int plba, psize;
    if (!find_part(n, &plba, &psize)) return -1;
    ata_read_sector(plba + PART_RESERVED, disk_buf);
    if (!is_myfs()) return -2;
    Superblock* sb = (Superblock*)disk_buf;
    unsigned int table = sb->table_lba;
    unsigned int dptr  = sb->data_ptr;               // запомним: disk_buf станет буфером копирования
    unsigned short fcnt = sb->file_count;
    ata_read_sector(table, sec_buf);
    int vi = -1;                                     // индекс удаляемой записи
    for (int i = 0; i < FS_FILES_MAX; i++) {
        FileEntry* f = (FileEntry*)(sec_buf + i * 64);
        if (f->name[0] && str_eq(f->name, name)) { vi = i; break; }
    }
    if (vi < 0) return -3;
    FileEntry* v = (FileEntry*)(sec_buf + vi * 64);
    unsigned int vsec = (v->size + 511) / 512;       // сколько секторов занимал файл
    // сдвигаем данные всех файлов, лежавших после него
    for (int i = 0; i < FS_FILES_MAX; i++) {
        FileEntry* f = (FileEntry*)(sec_buf + i * 64);
        if (i == vi || f->name[0] == 0 || f->start <= v->start) continue;
        if (vsec > 0) {
            unsigned int fs = (f->size + 511) / 512;
            for (unsigned int s = 0; s < fs; s++) {  // сектор за сектором (копируем «вниз»)
                ata_read_sector(f->start + s, disk_buf);
                ata_write_sector(f->start + s - vsec, disk_buf);
            }
            f->start -= vsec;
        }
    }
    for (int i = 0; i < 64; i++) ((unsigned char*)v)[i] = 0;   // запись удалена
    ata_write_sector(table, sec_buf);
    ata_read_sector(plba + PART_RESERVED, disk_buf); // суперблок: читаем заново после движений
    sb = (Superblock*)disk_buf;
    sb->data_ptr   = dptr - vsec;
    sb->file_count = (unsigned short)(fcnt - 1);
    ata_write_sector(plba + PART_RESERVED, disk_buf);
    return 0;
}

// Переименовать файл oldname → newname в разделе N.
// Возврат: 0 — ок, -1 — нет раздела, -2 — не MyFS, -3 — нет файла, -4 — имя занято.
int fs_rename(int n, const char* oldname, const char* newname) {
    unsigned int plba, psize;
    if (!find_part(n, &plba, &psize)) return -1;
    ata_read_sector(plba + PART_RESERVED, disk_buf);
    if (!is_myfs()) return -2;
    Superblock* sb = (Superblock*)disk_buf;
    ata_read_sector(sb->table_lba, sec_buf);
    FileEntry* found = 0;
    for (int i = 0; i < FS_FILES_MAX; i++) {
        FileEntry* f = (FileEntry*)(sec_buf + i * 64);
        if (f->name[0] == 0) continue;
        if (str_eq(f->name, newname) && !str_eq(f->name, oldname)) return -4;
        if (str_eq(f->name, oldname)) found = f;
    }
    if (!found) return -3;
    for (int i = 0; i < 56; i++) found->name[i] = 0;
    int k = 0;
    while (newname[k] && k < 55) { found->name[k] = newname[k]; k++; }
    ata_write_sector(sb->table_lba, sec_buf);
    return 0;
}

// Показать содержимое файла name из раздела N
void fs_cat(int n, const char* name) {
    unsigned int plba, psize;
    if (!find_part(n, &plba, &psize)) { cmd_print("No such partition\n"); return; }
    ata_read_sector(plba + PART_RESERVED, disk_buf);
    if (!is_myfs()) { cmd_print("Not MyFS (format it first)\n"); return; }
    Superblock* sb = (Superblock*)disk_buf;
    ata_read_sector(sb->table_lba, sec_buf);
    FileEntry* found = 0;
    for (int i = 0; i < FS_FILES_MAX; i++) {
        FileEntry* f = (FileEntry*)(sec_buf + i * 64);
        if (f->name[0] && str_eq(f->name, name)) { found = f; break; }
    }
    if (!found) { cmd_print("No such file\n"); return; }
    unsigned int left = found->size;
    unsigned int lba  = found->start;
    while (left > 0) {
        ata_read_sector(lba, disk_buf);               // суперблок уже не нужен
        int chunk = (left < 512) ? (int)left : 512;
        for (int i = 0; i < chunk; i++) cmd_putchar((char)disk_buf[i]);
        left -= (unsigned int)chunk;
        lba++;
    }
    cmd_putchar('\n');
}

// ===========================================================================
//  ПОДДЕРЖКА ПРИЛОЖЕНИЙ: сборка строк и чтение файлов для окон
// ===========================================================================
//  sprintf у нас нет, поэтому строки собираем вручную: sk_str дописывает
//  текст, sk_num — число, sk_hex8 — байт в hex (для адресов). Все возвращают
//  новую длину буфера — удобно собирать длинные строки по кусочкам.
int sk_str(char* b, int k, const char* s) { while (*s) b[k++] = *s++; return k; }
int sk_num(char* b, int k, unsigned int v) {
    char t[12]; int i = 0;
    if (v == 0) t[i++] = '0';
    while (v > 0) { t[i++] = (char)('0' + v % 10); v /= 10; }
    while (i > 0) b[k++] = t[--i];
    return k;
}
int sk_hex8(char* b, int k, unsigned char v) {
    const char* h = "0123456789ABCDEF";
    b[k++] = h[(v >> 4) & 0x0F];
    b[k++] = h[v & 0x0F];
    return k;
}
// 4 байта (32-битное число) в hex — для адресов ОЗУ
int sk_hex32(char* b, int k, unsigned int v) {
    k = sk_hex8(b, k, (unsigned char)((v >> 24) & 0xFF));
    k = sk_hex8(b, k, (unsigned char)((v >> 16) & 0xFF));
    k = sk_hex8(b, k, (unsigned char)((v >> 8) & 0xFF));
    return sk_hex8(b, k, (unsigned char)(v & 0xFF));
}

// Список файлов для проводника: копия таблицы MyFS выбранного раздела
struct FileInfo { char name[56]; unsigned int size; unsigned int start; };
FileInfo file_list[FS_FILES_MAX];

// Загрузить таблицу файлов раздела n в file_list.
// Возврат: сколько файлов (>0), 0 — пусто, -1 — нет раздела, -2 — не MyFS.
int fs_load_table(int n) {
    unsigned int plba, psize;
    if (!find_part(n, &plba, &psize)) return -1;
    ata_read_sector(plba + PART_RESERVED, disk_buf);
    if (!is_myfs()) return -2;
    Superblock* sb = (Superblock*)disk_buf;
    ata_read_sector(sb->table_lba, sec_buf);
    int cnt = 0;
    for (int i = 0; i < FS_FILES_MAX; i++) {
        FileEntry* f = (FileEntry*)(sec_buf + i * 64);
        if (f->name[0] == 0) continue;               // пустая запись
        for (int k = 0; k < 56; k++) file_list[cnt].name[k] = f->name[k];
        file_list[cnt].size  = f->size;
        file_list[cnt].start = f->start;
        cnt++;
    }
    return cnt;
}

// Буфер содержимого файла для окна просмотра (показываем не больше VIEW_MAX)
const int VIEW_MAX = 2048;
char view_buf[VIEW_MAX];
unsigned int view_len = 0;

// Прочитать файл (start/size из file_list) в view_buf — посекторно, как cat
void fs_read_file(unsigned int start, unsigned int size) {
    view_len = 0;
    if (size > VIEW_MAX) size = VIEW_MAX;            // большие файлы — первые 2 КБ
    while (size > 0) {
        ata_read_sector(start, disk_buf);
        unsigned int chunk = (size < 512) ? size : 512;
        for (unsigned int i = 0; i < chunk; i++) view_buf[view_len++] = (char)disk_buf[i];
        size -= chunk;
        start++;
    }
}

// Прочитать файл №idx (из списка file_list, см. fs_load_table) в буфер dst.
// Не больше max байт. Возврат: сколько байт прочитано.
int fs_read_content(int idx, char* dst, int max) {
    unsigned int start = file_list[idx].start;
    unsigned int size  = file_list[idx].size;
    if ((int)size > max) size = (unsigned int)max;
    int got = 0;
    while (size > 0) {
        ata_read_sector(start, disk_buf);
        unsigned int chunk = (size < 512) ? size : 512;
        for (unsigned int i = 0; i < chunk; i++) dst[got++] = (char)disk_buf[i];
        size  -= chunk;
        start++;
    }
    return got;
}

// ===========================================================================
//  УСТАНОВКА СИСТЕМЫ В РАЗДЕЛ — команда install N [yes]
// ===========================================================================
//  Копирует систему из НАЧАЛА диска (откуда она собиралась) в служебную
//  зону раздела N (первые PART_RESERVED секторов):
//    сектор 1 диска (VBR)      -> +0  раздела
//    сектора 2-3 диска (stage2)-> +1  раздела (2 сектора)
//    сектора 4+ диска (ядро)   -> +3  раздела (KERNEL_SECTORS штук)
//
//  VBR не знает, в каком разделе лежит, поэтому установщик ВПЕЧАТЫВАЕТ в его
//  копию LBA раздела — 4 байта по смещению 504 (поле base_lba, см. vbr.asm).
//  По этому полю VBR (и потом stage2) находят свои stage2 и ядро.
//
//  БЕЗОПАСНОСТЬ: без слова yes ничего не пишем — сначала показываем план.
//  MyFS живёт с сектора +128, а install пишет только начало — файлы не задеваются.
// Проверить раздел для установки; при отказе печатает причину.
// true = можно ставить, *plba/*psize заполнены.
bool install_check(int n, unsigned int* plba, unsigned int* psize) {
    if (!find_part(n, plba, psize)) { cmd_print("No such partition\n"); return false; }
    if (*psize < PART_RESERVED + 3) { cmd_print("Partition too small\n"); return false; }
    if (KERNEL_SECTORS + 3 > PART_RESERVED) {
        cmd_print("Kernel too big for boot area\n"); return false;
    }
    return true;
}

// Напечатать план установки в раздел n (общий код для install и setup)
void install_plan(int n, unsigned int plba, unsigned int psize) {
    cmd_print("Install to P");
    cmd_putchar((char)('0' + n));
    cmd_print(": lba=");  print_dec(plba);
    cmd_print(" size=");  print_dec(psize / 2048); cmd_print("MB\n");
    cmd_print("Boot area: first 128 sec\n");
    cmd_print("  boot 1 + stage2 2 + kernel ");
    print_dec(KERNEL_SECTORS);
    cmd_print("\n");
    cmd_print("MyFS files stay untouched.\n");
}

// Сама установка (без проверок и вопросов): копируем и помечаем активным.
// talk = false — не печатать шаги в консоль (мастер показывает их в окне).
void install_do(int n, unsigned int plba, bool talk = true) {
    if (talk) {
        cmd_print("Installing to P");
        cmd_putchar((char)('0' + n));
        cmd_print("...\n");
    }

    // --- 1. VBR: сектор 1 диска + впечатать LBA раздела (поле на +504) ---
    ata_read_sector(1, disk_buf);
    disk_buf[504] = (unsigned char)(plba & 0xFF);      // LBA раздела (u32, LE)
    disk_buf[505] = (unsigned char)((plba >> 8) & 0xFF);
    disk_buf[506] = (unsigned char)((plba >> 16) & 0xFF);
    disk_buf[507] = (unsigned char)((plba >> 24) & 0xFF);
    ata_write_sector(plba, disk_buf);
    if (talk) { cmd_print("  VBR -> "); print_dec(plba); cmd_print("\n"); }

    // --- 2. stage2 (2 сектора): сектора 2-3 диска -> +1..+2 раздела ---
    ata_read_sector(2, disk_buf);
    ata_write_sector(plba + 1, disk_buf);
    ata_read_sector(3, disk_buf);
    ata_write_sector(plba + 2, disk_buf);
    if (talk) { cmd_print("  stage2 2 sec -> "); print_dec(plba + 1); cmd_print("\n"); }

    // --- 3. ядро: сектора 4+ диска -> +3.. раздела (читаем и пишем посекторно) ---
    for (unsigned int i = 0; i < KERNEL_SECTORS; i++) {
        ata_read_sector(4 + i, disk_buf);
        ata_write_sector(plba + 3 + i, disk_buf);
    }
    if (talk) {
        cmd_print("  kernel "); print_dec(KERNEL_SECTORS);
        cmd_print(" sec -> "); print_dec(plba + 3); cmd_print("\n");
    }

    // --- 4. Раздел N становится АКТИВНЫМ (флаг 0x80), остальные снимаем ---
    ata_read_sector(0, disk_buf);
    for (int i = 0; i < 4; i++) {
        unsigned char* e = disk_buf + 446 + i * 16;
        if (e[4] == 0) continue;                       // пустой слот не трогаем
        e[0] = (i == n - 1) ? 0x80 : 0x00;
    }
    ata_write_sector(0, disk_buf);

    if (talk) {
        cmd_print("P"); cmd_putchar((char)('0' + n));
        cmd_print(" marked active. Done.\n");
    }
}

// Команда install N [yes]: без yes — только план, с yes — установка
void cmd_install(int n, bool confirm) {
    unsigned int plba, psize;
    if (!install_check(n, &plba, &psize)) return;
    install_plan(n, plba, psize);
    if (!confirm) {
        cmd_print("Type: install ");
        cmd_putchar((char)('0' + n));
        cmd_print(" yes\n");
        return;
    }
    install_do(n, plba);
}

// ===========================================================================
//  ГРАФИЧЕСКИЙ МАСТЕР УСТАНОВКИ — окно «Installer» (команда setup)
// ===========================================================================
//  Шаги мастера (inst_state):
//    0 — выбрать раздел (стрелки/мышь)
//    1 — ПОДТВЕРЖДЕНИЕ: показываем план и ждём Enter/Install
//    2 — запись (показывается один кадр «Writing...»)
//    3 — готово
//  Правило безопасности то же, что у команды install N: сначала показываем
//  план, и только после подтверждения пишем. MyFS не трогаем — он живёт
//  дальше по разделу, а мы пишем лишь зону загрузки.
//  Мышь: щелчок по строке — выбрать, по кнопке — нажать.
//  Клавиши: Up/Down — раздел, Tab/Left/Right — кнопка, Enter — нажать,
//           Esc/F3 — назад (из подтверждения) или закрыть окно.
bool inst_active = false;                // окно мастера открыто?
int  inst_state = 0;                     // шаг мастера (см. выше)
int  inst_row   = 0;                     // строка списка под курсором (0..3)
int  inst_part  = 0;                     // выбранный раздел 1..4 (0 — нет)
int  inst_btn   = 0;                     // кнопка: 0 = Install, 1 = Cancel/Close
unsigned int inst_lba[4];                // LBA разделов из MBR
unsigned int inst_mb[4];                 // размеры разделов в МБ
int  inst_fl[4];                         // биты: 1 занят, 2 MyFS, 4 можно ставить, 8 активный
const char* inst_note = 0;               // замечание мастера (снимается клавишей/щелчком)

const int INST_ROW0  = 46;               // y первой строки списка (внутри окна)
const int INST_ROWH  = 30;               // шаг строк списка
const int INST_BTN_W = 110;              // ширина кнопки
const int INST_BTN_H = 26;               // высота кнопки

// Прямоугольник кнопки i (0 = Install, 1 = Cancel) внутри окна мастера
void inst_btn_rect(int i, int* bx, int* by) {
    int w = win_wp[APP_INSTALL], h = win_hp[APP_INSTALL];
    *by = h - INST_BTN_H - 14;
    *bx = (i == 0) ? (w - INST_BTN_W - 16) : (w - INST_BTN_W * 2 - 26);
}

// Перечитать таблицу разделов: LBA, размер и флаги (MyFS / активный / годный)
void inst_scan() {
    for (int i = 0; i < 4; i++) { inst_lba[i] = 0; inst_mb[i] = 0; inst_fl[i] = 0; }
    ata_read_sector(0, disk_buf);            // MBR
    for (int i = 0; i < 4; i++) {
        unsigned char* e = disk_buf + 446 + i * 16;
        if (e[4] == 0) continue;             // пустой слот
        // Данные записи вынимаем СРАЗУ: буфер ещё пригодится под суперблок MyFS
        unsigned int plba  = (unsigned int)e[8]  | ((unsigned int)e[9]  << 8) |
                             ((unsigned int)e[10] << 16) | ((unsigned int)e[11] << 24);
        unsigned int psize = (unsigned int)e[12] | ((unsigned int)e[13] << 8) |
                             ((unsigned int)e[14] << 16) | ((unsigned int)e[15] << 24);
        inst_lba[i] = plba;
        inst_mb[i]  = psize / 2048;
        int fl = 1;                          // слот занят
        if (e[0] == 0x80) fl |= 8;           // он же активный
        if (psize >= PART_RESERVED + 3 && KERNEL_SECTORS + 3 <= PART_RESERVED) fl |= 4;
        disk_buf[0] = 0;                     // чтобы мусор не дал ложный «MyFS»
        ata_read_sector(plba + PART_RESERVED, disk_buf);   // суперблок MyFS?
        if (disk_buf[0] == 'M' && disk_buf[1] == 'Y' &&
            disk_buf[2] == 'F' && disk_buf[3] == 'S') fl |= 2;
        ata_read_sector(0, disk_buf);        // вернуть MBR в буфер
        inst_fl[i] = fl;
    }
}

// Открыть окно мастера (команда setup)
void inst_open() {
    if (inst_active) { win_raise(APP_INSTALL); draw_desktop(); return; }
    inst_active = true;
    inst_state  = 0;
    inst_row    = 0;
    inst_part   = 0;
    inst_btn    = 0;
    inst_note   = 0;
    inst_scan();
    win_place(APP_INSTALL, win_w(620), win_h(330));
    win_raise(APP_INSTALL);
    draw_desktop();
}

// Закрыть окно мастера (кнопка Cancel/Close, Esc, красная кнопка)
void inst_close() {
    inst_active = false;
    win_close_app(APP_INSTALL);
    draw_desktop();
}

// Одна строка списка: «P1  lba=2048  30608 MB  ACTIVE  MyFS»
void inst_draw_row(int i, int x, int y, int w, bool sel) {
    int fl = inst_fl[i];
    unsigned int col = sel ? C_BLACK : TEXT_COL;
    if (sel) fill_rect(x, y, w, INST_ROWH - 2, C_WHITE);   // выделение — белая плашка
    char b[64]; int k = 0;
    k = sk_str(b, k, "P"); k = sk_num(b, k, (unsigned int)(i + 1)); b[k] = 0;
    draw_text(x + 10, y + 6, b, col);
    if (!(fl & 1)) { draw_text(x + 40, y + 6, "(empty slot)", col); return; }
    k = sk_str(b, 0, "lba="); k = sk_num(b, k, inst_lba[i]);
    k = sk_str(b, k, "   ");  k = sk_num(b, k, inst_mb[i]); k = sk_str(b, k, " MB");
    b[k] = 0;
    draw_text(x + 40, y + 6, b, col);
    int tx = x + 320;
    if (fl & 8) { draw_text(tx, y + 6, "ACTIVE", col); tx += 56; }
    if (fl & 2) { draw_text(tx, y + 6, "MyFS", col);   tx += 48; }
    if (!(fl & 4)) draw_text(tx, y + 6, "too small", col);
}

// Нарисовать окно мастера целиком (зовёт композитор draw_app)
void draw_install() {
    int x = win_x[APP_INSTALL], y = win_y[APP_INSTALL];
    int w = win_wp[APP_INSTALL], h = win_hp[APP_INSTALL];
    draw_window_frame(x, y, w, h, "Installer");
    int tx = x + 12, ty = y + TITLE_H + 6;
    draw_text(tx, ty, "MyOS Setup - pick a partition (MyFS files are kept)", TEXT_COL);
    for (int i = 0; i < 4; i++)
        inst_draw_row(i, x + 8, y + INST_ROW0 + i * INST_ROWH, w - 16, i == inst_row);
    // --- сообщения мастера под списком ---
    int sy = y + INST_ROW0 + 4 * INST_ROWH + 4;
    dither_over(x + 12, sy, w - 24, 1, C_WHITE, 7);            // тонкая линия
    char b[96]; int k = 0;
    if (inst_note) {
        k = sk_str(b, k, inst_note); b[k] = 0;
        draw_text(tx, sy + 8, b, TEXT_COL);
    } else if (inst_state == 0) {
        draw_text(tx, sy + 8,  "Up/Down or mouse picks a partition.", TEXT_COL);
        draw_text(tx, sy + 24, "Install shows the plan first - nothing is written yet.", TEXT_COL);
    } else if (inst_state == 1) {
        k = sk_str(b, k, "Write to P"); k = sk_num(b, k, (unsigned int)inst_part);
        k = sk_str(b, k, " (lba=");     k = sk_num(b, k, inst_lba[inst_part - 1]);
        k = sk_str(b, k, ", ");         k = sk_num(b, k, inst_mb[inst_part - 1]);
        k = sk_str(b, k, " MB)?"); b[k] = 0;
        draw_text(tx, sy + 8, b, TEXT_COL);
        k = 0;
        k = sk_str(b, k, "Boot area: boot 1 + stage2 2 + kernel ");
        k = sk_num(b, k, KERNEL_SECTORS);
        k = sk_str(b, k, " sec. MyFS files stay untouched."); b[k] = 0;
        draw_text(tx, sy + 24, b, TEXT_COL);
        draw_text(tx, sy + 40, "Enter = write now, Esc = back", TEXT_COL);
    } else if (inst_state == 2) {
        draw_text(tx, sy + 8, "Writing boot area, stage2 and kernel...", TEXT_COL);
    } else {
        k = sk_str(b, k, "Done! P"); k = sk_num(b, k, (unsigned int)inst_part);
        k = sk_str(b, k, " is ACTIVE. Reboot and MyOS boots from it."); b[k] = 0;
        draw_text(tx, sy + 8, b, TEXT_COL);
        draw_text(tx, sy + 24, "MyFS files stay untouched.", TEXT_COL);
    }
    // --- кнопки: выбранная — белая плашка с чёрным текстом ---
    for (int i = 0; i < 2; i++) {
        int bx, by; inst_btn_rect(i, &bx, &by);
        bx += x; by += y;
        const char* label = (i == 0) ? "Install" : (inst_state == 3 ? "Close" : "Cancel");
        int n = 0; while (label[n]) n++;
        int lx = bx + (INST_BTN_W - n * 8) / 2;
        if (i == inst_btn) {
            fill_rect(bx, by, INST_BTN_W, INST_BTN_H, C_WHITE);
            draw_text(lx, by + 5, label, C_BLACK);
        } else {
            draw_rect(bx, by, INST_BTN_W, INST_BTN_H, C_WHITE);
            draw_text(lx, by + 5, label, TEXT_COL);
        }
    }
}

// Нажали Install: шаг 0 -> «показать план», шаг 1 -> ПИСАТЬ, шаг 3 -> закрыть
void inst_activate() {
    if (inst_state == 3) { inst_close(); return; }              // после установки — «Close»
    if (inst_state == 1) {                                      // подтверждено — ПИШЕМ
        unsigned int plba, psize;
        if (!install_check(inst_part, &plba, &psize)) {
            inst_note = "This partition cannot be used";
            inst_state = 0;
            draw_desktop();
            return;
        }
        inst_state = 2;                                         // кадр «Writing...»
        draw_desktop();
        install_do(inst_part, plba, false);                     // VBR + stage2 + ядро
        inst_scan();                                            // перечитали ACTIVE
        inst_state = 3;
        inst_btn = 1;                                           // дальше логичнее «Close»
        draw_desktop();
        return;
    }
    int fl = inst_fl[inst_row];                                 // шаг 0: годен ли раздел?
    if (!(fl & 1))      inst_note = "That slot is empty - pick another partition";
    else if (!(fl & 4)) inst_note = "Partition too small for the boot area";
    else { inst_part = inst_row + 1; inst_state = 1; inst_note = 0; }  // показываем план
    draw_desktop();
}

// Щелчок мышью внутри окна мастера (dx, dy — координаты ВНУТРИ окна)
void inst_click(int dx, int dy) {
    if (inst_state == 2) return;                                // идёт запись — не мешаем
    for (int i = 0; i < 4; i++) {                               // строка списка?
        int ry = INST_ROW0 + i * INST_ROWH;
        if (dy >= ry && dy < ry + INST_ROWH - 2) {
            inst_note = 0;
            if (inst_state == 1) inst_state = 0;                // сменили раздел — снова выбор
            inst_row = i;
            draw_desktop();
            return;
        }
    }
    for (int i = 0; i < 2; i++) {                               // кнопка?
        int bx, by; inst_btn_rect(i, &bx, &by);
        if (dx >= bx && dx < bx + INST_BTN_W && dy >= by && dy < by + INST_BTN_H) {
            inst_btn = i;
            if (i == 1) inst_close();                           // Cancel / Close
            else        inst_activate();                        // Install
            return;
        }
    }
}

// Клавиши в окне мастера
void inst_handle_key(int sc) {
    if (sc == 0xE0) { e0_prefix = true; return; }
    bool ext = e0_prefix; e0_prefix = false;
    if (sc & 0x80) return;                                      // отпускания не нужны
    if (inst_note) { inst_note = 0; draw_desktop(); }           // замечание снимается
    if (inst_state == 2) return;                                // идёт запись — подождём
    if (ext) {                                                  // стрелки
        if (sc == 0x48 && inst_row > 0) {                       // Up
            inst_row--; if (inst_state == 1) inst_state = 0; draw_desktop();
        }
        if (sc == 0x50 && inst_row < 3) {                       // Down
            inst_row++; if (inst_state == 1) inst_state = 0; draw_desktop();
        }
        if (sc == 0x4B || sc == 0x4D) { inst_btn = 1 - inst_btn; draw_desktop(); }  // L/R
        return;
    }
    if (sc == 0x0F) { inst_btn = 1 - inst_btn; draw_desktop(); return; }   // Tab
    if (sc == 0x1C) { inst_btn = 0; inst_activate(); return; }             // Enter
    if (sc == 0x01 || sc == SC_F3) {                                       // Esc / F3
        if (inst_state == 1) { inst_state = 0; draw_desktop(); return; }   // назад
        inst_close();
    }
}

// --- Разбор аргументов команд (для format / ls / write / cat) ---
// Пропустить слово-команду и пробелы после него
const char* skip_word(const char* p) {
    while (*p && *p != ' ') p++;
    while (*p == ' ') p++;
    return p;
}
// Прочитать десятичное число; сдвинуть указатель за него и пробелы
unsigned int read_num(const char** pp) {
    const char* p = *pp;
    unsigned int v = 0;
    while (*p >= '0' && *p <= '9') v = v * 10 + (unsigned int)(*p++ - '0');
    while (*p == ' ') p++;
    *pp = p;
    return v;
}
// Прочитать слово (имя файла) в буфер out; сдвинуть указатель дальше
void read_word(const char** pp, char* out, int max) {
    const char* p = *pp;
    int i = 0;
    while (*p && *p != ' ' && i < max - 1) out[i++] = *p++;
    out[i] = 0;
    while (*p == ' ') p++;
    *pp = p;
}

void run_command(const char* line) {
    if (line[0] == 0) return;            // пустая команда
    if (str_eq(line, "help")) {
        cmd_print("Commands:\n");
        cmd_print("  help\n");
        cmd_print("  info\n");
        cmd_print("  clear\n");
        cmd_print("  mem\n");
        cmd_print("  time\n");
        cmd_print("  echo T\n");
        cmd_print("  part\n");
        cmd_print("  mkpart N MB\n");
        cmd_print("  format N\n");
        cmd_print("  ls N\n");
        cmd_print("  write N f T\n");
        cmd_print("  cat N f\n");
        cmd_print("  install N\n");
        cmd_print("  setup\n");
        cmd_print("  play\n");
        cmd_print("  code\n");
        cmd_print("  wtest\n");
        cmd_print("  ring3\n");
        cmd_print("  Up/Dn = history\n");
    } else if (str_eq(line, "play")) {
        game_open();                         // мини-игра «Змейка»
    } else if (str_eq(line, "code")) {
        code_open();                         // редактор кода MyC
    } else if (str_eq(line, "clear")) {
        fill_rect(TXT_X, TXT_Y, TXT_W, TXT_H, C_BLACK);        // снять текст
        cur_col = 0; cur_row = 0;
    } else if (str_eq(line, "info")) {
        cmd_print("MyOS v0.1 (build 0)\n");
        cmd_print("Video: VESA VBE (LFB)\n");
        cmd_print("Screen: ");
        print_dec(W); cmd_putchar('x'); print_dec(H); cmd_putchar('x'); print_dec(FB_BPP);
        cmd_print("\n");
        cmd_print("Boot: MBR -> VBR -> stage2\n");
        // Из какого раздела загрузились? Спрашиваем загрузчика — см. boot_partition
        unsigned short bootlba = 0;
        int bp = boot_partition(&bootlba);
        if (bp >= 0) {                       // «записка» от VBR есть
            cmd_print("Boot: P");
            cmd_putchar(bp > 0 ? (char)('0' + bp) : '?');
            cmd_print(" (lba=");
            print_dec(bootlba);
            cmd_print(")\n");
        } else {
            cmd_print("Boot: raw sectors\n");
        }
    } else if (str_eq(line, "mem")) {
        cmd_print("Memory map:\n");
        cmd_print("  Kernel   0x10000\n");
        cmd_print("  Stack    0x90000\n");
        cmd_print("  Video    0x");
        print_hex8((unsigned char)((unsigned int)fb >> 24));   // адрес кадрового
        print_hex8((unsigned char)((unsigned int)fb >> 16));   // буфера — какой
        print_hex8((unsigned char)((unsigned int)fb >> 8));    // дал VESA BIOS
        print_hex8((unsigned char)((unsigned int)fb));
        cmd_print("\n");
        cmd_print("  Conv mem 640 KB\n");
    } else if (str_eq(line, "time")) {
        int sec  = bcd2bin(cmos_read(0));    // регистры CMOS: сек/мин/час
        int min  = bcd2bin(cmos_read(2));
        int hour = bcd2bin(cmos_read(4));
        cmd_print("Time: ");
        print2(hour); cmd_putchar(':'); print2(min); cmd_putchar(':'); print2(sec);
        cmd_print("\n");
    } else if (str_eq(line, "part")) {
        cmd_part();                          // читаем MBR через ATA и печатаем разделы
    } else if (str_startswith(line, "mkpart ")) {
        // разбираем аргументы: "mkpart N MB"
        const char* p = line + 7;                       // пропускаем слово "mkpart "
        int n = 0, mb = 0;
        while (*p >= '0' && *p <= '9') n = n * 10 + (*p++ - '0');   // номер раздела
        while (*p == ' ') p++;                                      // пробелы
        while (*p >= '0' && *p <= '9') mb = mb * 10 + (*p++ - '0'); // размер, МБ
        cmd_mkpart(n, mb);
    } else if (str_eq(line, "mkpart")) {
        cmd_print("Usage: mkpart N MB  (N=1..4, MB=1..32)\n");
    } else if (str_startswith(line, "format ")) {
        const char* p = skip_word(line);
        fs_format((int)read_num(&p));
    } else if (str_startswith(line, "ls ")) {
        const char* p = skip_word(line);
        fs_ls((int)read_num(&p));
    } else if (str_startswith(line, "write ")) {
        const char* p = skip_word(line);
        int n = (int)read_num(&p);
        char name[24];
        read_word(&p, name, 24);
        if (name[0] == 0) cmd_print("Usage: write N file text\n");
        else fs_write(n, name, p);                    // p — остаток строки (текст)
    } else if (str_startswith(line, "cat ")) {
        const char* p = skip_word(line);
        int n = (int)read_num(&p);
        char name[24];
        read_word(&p, name, 24);
        if (name[0] == 0) cmd_print("Usage: cat N file\n");
        else fs_cat(n, name);
    } else if (str_startswith(line, "install ")) {
        const char* p = skip_word(line);
        int n = (int)read_num(&p);
        char word[8];
        read_word(&p, word, 8);
        cmd_install(n, str_eq(word, "yes"));   // без "yes" — только план, ничего не пишем
    } else if (str_eq(line, "install")) {
        cmd_print("Usage: install N [yes]\n");
    } else if (str_eq(line, "setup")) {
        inst_open();                         // графический мастер установки
    } else if (str_eq(line, "ring3")) {
        if (user_start(user_demo, user_demo_len))   // программа в ring 3 (отдельный бинарник)
            draw_desktop();                  // стереть её картинку, вернуть рабочий стол
    } else if (str_eq(line, "wtest")) {
        cmd_wtest();                         // тест записи: write + read + сверка
    } else if (str_startswith(line, "echo ")) {
        cmd_print(line + 5);
    } else if (str_eq(line, "echo")) {
        cmd_print("(nothing to echo)\n");
    } else {
        cmd_print("Unknown: ");
        cmd_print(line);
        cmd_print("\n");
    }
}

// ===========================================================================
//  РАБОЧИЙ СТОЛ: меню-бар, живые часы и док приложений
//  (Terminal, Files, Settings, Tasks, About — окна см. ниже)
// ===========================================================================
//  Стиль в нашем чёрно-белом ключе: тонкие белые контуры на чёрном фоне.
//  Меню-бар сверху: слева «MyOS», справа — часы (из микросхемы CMOS).
//  Док снизу: иконки приложений. Стрелки Left/Right — выбор, Enter — запуск.

const int MB_H = 16;                     // высота меню-бара (ровно одна строка)
int last_clock_sec = -1;                 // секунда, показанная в прошлый раз

// Нарисовать часы в правом углу меню-бара: «18:42» или «6:42PM».
// Формат переключается в окне Settings (переменная clock_24h).
int clock_24h = 1;                       // 1 = 24 часа, 0 = 12 часов с AM/PM
void draw_clock() {
    int h = bcd2bin(cmos_read(4));       // рег. 4 = часы (BCD), см. cmos_read
    int m = bcd2bin(cmos_read(2));       // рег. 2 = минуты (BCD)
    char buf[10];
    int k = 0;
    if (clock_24h) {
        buf[k++] = (char)('0' + h / 10); buf[k++] = (char)('0' + h % 10);
        buf[k++] = ':';
        buf[k++] = (char)('0' + m / 10); buf[k++] = (char)('0' + m % 10);
    } else {
        bool pm = (h >= 12);
        int h12 = h % 12; if (h12 == 0) h12 = 12;
        if (h12 >= 10) buf[k++] = (char)('0' + h12 / 10);   // 10..12 — две цифры
        buf[k++] = (char)('0' + h12 % 10);
        buf[k++] = ':';
        buf[k++] = (char)('0' + m / 10); buf[k++] = (char)('0' + m % 10);
        buf[k++] = pm ? 'P' : 'A'; buf[k++] = 'M';
    }
    buf[k] = 0;
    fill_rect(W - 64, 0, 64, 16, C_BLACK);              // стереть старые цифры
    draw_text(W - 62, 0, buf, TEXT_COL);               // 7 знаков максимум
}

// Вызывается в главном цикле: CMOS оттикал новую секунду — обновить часы
void clock_tick() {
    int s = bcd2bin(cmos_read(0));       // регистр 0 = секунды
    if (s == last_clock_sec) return;     // та же секунда — экран не трогаем
    last_clock_sec = s;
    cursor_hide();                       // часы могут попасть под курсор
    draw_clock();
    cursor_show();
}

// Меню-бар: чёрная полоса во всю ширину + белая линия-разделитель снизу
void draw_menubar() {
    fill_rect(0, 0, W, MB_H, C_BLACK);              // полоса меню-бара (сплошная)
    draw_text(6, 0, "MyOS", TEXT_COL);
    for (int x = 0; x < W; x++)                     // тонкая белая линия под полосой
        put_pixel(x, MB_H, C_WHITE);
    draw_clock();
}

// --- Док (панель приложений внизу экрана) ---
const int DOCK_N = 7;                            // сколько приложений в доке
const int ICON_SZ = 28, ICON_GAP = 10, DOCK_PAD = 8;
const int DOCK_W = 2 * DOCK_PAD + DOCK_N * ICON_SZ + (DOCK_N - 1) * ICON_GAP;
const int DOCK_H = ICON_SZ + 2 * DOCK_PAD;
int DOCK_X = (320 - DOCK_W) / 2;                 // по центру (обновит init_desktop_layout)
int DOCK_Y = 200 - DOCK_H - 6;                   // небольшой отступ снизу
int dock_sel = 0;                                // какая иконка выбрана
const char* dock_names[DOCK_N] = { "Terminal", "Files", "Settings", "Tasks", "Code", "Game", "About" };

// Одна иконка: чёрный фон, белая рамка, символ приложения.
// Выбранная — дополнительно в двойной рамке.
// --- Иконки дока: картинки 20x20 пикселей (сгенерировал icons_gen.ps1) ---
#include "icons_data.inc"

// Нарисовать иконку 20x20 из битовой карты: 3 байта на строку,
// старший бит = САМЫЙ ЛЕВЫЙ пиксель строки.
void draw_icon20(int x, int y, const unsigned char* bmp, unsigned int color) {
    for (int j = 0; j < 20; j++) {
        unsigned int bits = ((unsigned int)bmp[j * 3] << 16) |
                            ((unsigned int)bmp[j * 3 + 1] << 8) |
                             (unsigned int)bmp[j * 3 + 2];
        if (!bits) continue;                       // пустая строка — пропуск
        for (int i = 0; i < 20; i++)
            if (bits & (0x800000u >> i)) put_pixel(x + i, y + j, color);
    }
}

void draw_dock_icon(int i) {
    int ix = DOCK_X + DOCK_PAD + i * (ICON_SZ + ICON_GAP);
    int iy = DOCK_Y + DOCK_PAD;
    fill_glass(ix, iy, ICON_SZ, ICON_SZ, 8, C_GLASS_ICON, GLASS_ICON);  // подложка
    draw_round_rect(ix, iy, ICON_SZ, ICON_SZ, 8, C_WHITE);              // рамка
    if (i == dock_sel)                                                  // выделение
        draw_round_rect(ix - 3, iy - 3, ICON_SZ + 6, ICON_SZ + 6, 10, C_WHITE);
    draw_icon20(ix + 4, iy + 4, dock_icons[i], TEXT_COL);               // картинка
}

// Подпись выбранного приложения — по центру экрана, над доком
void draw_dock_label() {
    erase_to_wallpaper(0, DOCK_Y - 18, W, 16);             // стереть старую подпись
    const char* s = dock_names[dock_sel];
    int n = 0; while (s[n]) n++;                           // длина названия
    draw_text((W - n * 8) / 2, DOCK_Y - 18, s, TEXT_COL);  // центрируем
}

// Весь док: стеклянная плашка + иконки + подпись
void draw_dock() {
    fill_glass(DOCK_X, DOCK_Y, DOCK_W, DOCK_H, 12, C_GLASS_DOCK, GLASS_DOCK);
    draw_round_rect(DOCK_X, DOCK_Y, DOCK_W, DOCK_H, 12, C_WHITE);    // её рамка
    for (int i = 0; i < DOCK_N; i++) draw_dock_icon(i);
    draw_dock_label();
}

// Перерисовать ТОЛЬКО док (плашку и подпись). Окна до дока не достают — их
// положение ограничено сверху, поэтому под доком всегда обои, и стереть его
// область «до обоев» безопасно. Нужно для подсветки иконок: полный кадр на
// каждое движение мыши не нужен — из-за него экран мерцал и тормозил.
void draw_dock_area() {
    erase_to_wallpaper(0, DOCK_Y - 20, W, H - (DOCK_Y - 20));
    comp_full = true;                        // док рисуем «стеклом», как в кадре
    draw_dock();
    comp_full = false;
}

// Рабочий стол целиком, с нуля: обои + меню-бар + окна (по слоям) + док.
// Тело — в конце файла (draw_desktop_real), когда известны все окна.

// --- Окно «About» (справка о системе) ---
bool about_active = false;               // открыто ли окно About

// Из какого раздела загрузились? VBR оставил «записку» в низкой памяти:
// 0x0500 = LBA раздела (word), 0x0502 = сигнатура 'MY' (0x4D59).
// Возврат: 1..4 — номер раздела, 0 — записка есть, но раздела нет, -1 — не через VBR.
int boot_partition(unsigned short* out_lba) {
    if (*(volatile unsigned short*)0x0502 != 0x4D59) return -1;
    unsigned short bootlba = *(volatile unsigned short*)0x0500;
    if (out_lba) *out_lba = bootlba;
    ata_read_sector(0, disk_buf);        // читаем MBR — ищем раздел с таким LBA
    for (int i = 0; i < 4; i++) {
        unsigned char* e = disk_buf + 446 + i * 16;
        if (e[4] == 0) continue;         // пустой слот
        unsigned int l = (unsigned int)e[8] | ((unsigned int)e[9] << 8) |
                         ((unsigned int)e[10] << 16) | ((unsigned int)e[11] << 24);
        if (l == bootlba) return i + 1;  // нашли — это наш раздел
    }
    return 0;
}

// Геометрия окна About — по центру экрана (обновит init_desktop_layout)
const int ABT_W = 210, ABT_H = 94;
int ABT_X = (320 - ABT_W) / 2;
int ABT_Y = 52;

// Пересчитать позиции дока и окна About под ТЕКУЩИЙ экран.
// Вызывается после init_layout() — когда W/H уже известны точно.
void init_desktop_layout() {
    DOCK_X = (W - DOCK_W) / 2;
    DOCK_Y = H - DOCK_H - 6;
    ABT_X = (W - ABT_W) / 2;
    ABT_Y = (H - ABT_H) / 2;
}

// Нарисовать окно «About MyOS» поверх рабочего стола
void draw_about() {
    draw_window_frame(ABT_X, ABT_Y, ABT_W, ABT_H, "About MyOS");
    int tx = ABT_X + 10;                 // левый край текста
    int ty = ABT_Y + TITLE_H + 8;        // первая строка — сразу под шапкой
    draw_text(tx, ty, "MyOS v0.1 (build 0)", TEXT_COL);

    // Строка о загрузке: «Boot: P1 (lba=2048)» — собираем вручную (нет sprintf)
    unsigned short lba = 0;
    int p = boot_partition(&lba);
    char buf[32];
    int k = 0;
    const char* pre = "Boot: P";
    while (pre[k]) { buf[k] = pre[k]; k++; }
    if (p > 0) {
        buf[k++] = (char)('0' + p);      // номер раздела
        const char* mid = " (lba=";
        for (int i = 0; mid[i]; i++) buf[k++] = mid[i];
        char num[8]; int n = 0;          // число LBA: цифры получаются задом наперёд...
        unsigned short v = lba;
        if (v == 0) num[n++] = '0';
        while (v > 0) { num[n++] = (char)('0' + v % 10); v /= 10; }
        while (n > 0) buf[k++] = num[--n];   // ...а выводим в прямом порядке
        buf[k++] = ')';
    } else {
        const char* alt = (p == 0) ? "? (not in table)" : " (raw boot)";
        for (int i = 0; alt[i]; i++) buf[k++] = alt[i];
    }
    buf[k] = 0;
    draw_text(tx, ty + 16, buf, TEXT_COL);

    // Строка о видео: «Video: 1024x768x32» — числа собираем вручную (нет sprintf)
    k = 0;
    const char* vpre = "Video: ";
    for (int i = 0; vpre[i]; i++) buf[k++] = vpre[i];
    unsigned int vv[3]; vv[0] = (unsigned int)W; vv[1] = (unsigned int)H; vv[2] = (unsigned int)FB_BPP;
    for (int ni = 0; ni < 3; ni++) {
        char num2[8]; int n2 = 0;
        unsigned int v2 = vv[ni];
        if (v2 == 0) num2[n2++] = '0';
        while (v2 > 0) { num2[n2++] = (char)('0' + v2 % 10); v2 /= 10; }
        while (n2 > 0) buf[k++] = num2[--n2];
        if (ni < 2) buf[k++] = 'x';      // между числами — «x»
    }
    buf[k] = 0;
    draw_text(tx, ty + 32, buf, TEXT_COL);
    draw_text(tx, ty + 48, "Esc closes this window", TEXT_COL);
}

// Открыть окно About через менеджер окон
void about_open() {
    win_place(APP_ABOUT, ABT_W, ABT_H);
    win_raise(APP_ABOUT);
    about_active = true;
    draw_desktop();
}

// ===========================================================================
//  ПРИЛОЖЕНИЯ ДОКА: Проводник (Files), Настройки (Settings), Задачи (Tasks)
// ===========================================================================
//  Каждое окно устроено как About: чёрный фон, белая рамка, Esc закрывает.
//  Открываются из дока (стрелки + Enter). Размеры «ужимаются» под экран.

int win_w(int want) { return (want < W - 16) ? want : W - 16; }   // ширина окна
int win_h(int want) { return (want < H - 40) ? want : H - 40; }   // высота окна

// --- ПРОВОДНИК (Files): файлы MyFS и просмотр содержимого ---
bool files_active = false;           // открыто ли окно
int  files_part = 1;                 // какой раздел смотрим (запоминается между запусками)
int  files_sel = 0;                  // выбранная строка списка
int  files_n = 0;                    // результат fs_load_table (>0 файлов, <0 — ошибка)
bool files_view = false;             // true = показываем содержимое файла
int  files_view_top = 0;             // первая видимая строка текста
int  files_text_rows = 1;            // сколько строк текста влезает в окно
int  fw, fh, fx, fy;                 // текущая геометрия окна (заполняет draw_files)

// Режим ввода/подтверждения: 0 — нет, 1 — имя нового файла, 2 — его текст,
// 3 — новое имя (переименование), 4 — подтверждение удаления, 5 — подтверждение формата
int  files_in = 0;
char fin_buf[64];                    // набираемый текст (fin_len знаков)
int  fin_len = 0;
char fin_name[56];                   // имя, введённое на первом шаге создания
char files_msg[56];                  // сообщение о результате операции ("" — нет)

// Переносы строк просмотра: где начинается каждая строка в view_buf
const int VLINE_MAX = 256;
unsigned int vline_off[VLINE_MAX];
int vline_n = 0;

// Разбить view_buf на строки: конец по '\n' или по ширине окна (cols знаков)
void files_wrap(int cols) {
    vline_n = 0;
    unsigned int i = 0;
    while (i < view_len && vline_n < VLINE_MAX) {
        vline_off[vline_n++] = i;
        int c = 0;
        while (i < view_len && c < cols) {
            if (view_buf[i] == '\n') { i++; break; }
            i++; c++;
        }
    }
}

// Загрузить список файлов выбранного раздела (сбрасывает режим просмотра)
void files_load() {
    files_view = false;
    files_view_top = 0;
    files_n = fs_load_table(files_part);
    int cnt = (files_n > 0) ? files_n : 0;
    if (files_sel >= cnt) files_sel = (cnt > 0) ? cnt - 1 : 0;
}

// Нарисовать окно проводника (список файлов или просмотр)
void draw_files() {
    fw = win_w(400); fh = win_h(300);
    win_wp[APP_FILES] = fw; win_hp[APP_FILES] = fh;      // окно двигается менеджером
    fx = win_x[APP_FILES]; fy = win_y[APP_FILES];

    // заголовок окна: «Files - P3» в списке, «P3: readme» в просмотре
    char t[48]; int k;
    if (!files_view) {
        k = sk_str(t, 0, "Files - P");
        k = sk_num(t, k, (unsigned int)files_part);
    } else {
        k = sk_str(t, 0, "P");
        k = sk_num(t, k, (unsigned int)files_part);
        k = sk_str(t, k, ": ");
        const char* nm = file_list[files_sel].name;
        int c = 0; while (nm[c] && c < 24) { t[k + c] = nm[c]; c++; }
        k += c;
    }
    t[k] = 0;
    draw_window_frame(fx, fy, fw, fh, t);

    int tx = fx + 10;                        // левый край текста
    int ty = fy + TITLE_H + 8;               // первая строка под шапкой
    int cols = (fw - 20) / 8;                // знаков в строке

    if (!files_view) {
        // --- список файлов ---
        if (files_n == -1) {
            draw_text(tx, ty, "No such partition", TEXT_COL);
        } else if (files_n == -2) {
            draw_text(tx, ty, "Not MyFS (console: format N)", TEXT_COL);
        } else {
            char h[24]; int hk = sk_str(h, 0, "Files: ");
            hk = sk_num(h, hk, (unsigned int)files_n);
            h[hk] = 0;
            draw_text(tx, ty, h, TEXT_COL);
            ty += 20;
            for (int i = 0; i < files_n; i++) {
                int y = ty + i * 18;
                unsigned int col = TEXT_COL;
                if (i == files_sel) {                    // выбранная — инверсия
                    fill_rect(tx - 2, y - 1, fw - 16, 16, C_WHITE);
                    col = C_BLACK;
                }
                draw_text(tx, y, file_list[i].name, col);
                char nb[12]; int nk = sk_num(nb, 0, file_list[i].size);
                nb[nk] = 0;
                draw_text(fx + fw - 12 - nk * 8, y, nb, col);   // размер у правого края
            }
            if (files_n == 0) draw_text(tx, ty, "(empty)", TEXT_COL);
        }
        // нижняя строка: подсказка, ввод операции или сообщение о результате
        char b[64]; int k = 0;
        if (files_in == 1 || files_in == 3) {            // ввод имени
            k = sk_str(b, k, "Name: ");
            for (int i = 0; i < fin_len; i++) b[k++] = fin_buf[i];
            b[k++] = '_'; b[k] = 0;
        } else if (files_in == 2) {                      // ввод текста
            k = sk_str(b, k, "Text: ");
            for (int i = 0; i < fin_len; i++) b[k++] = fin_buf[i];
            b[k++] = '_'; b[k] = 0;
        } else if (files_in == 4) {                      // подтверждение удаления
            k = sk_str(b, k, "Delete '");
            const char* nm = file_list[files_sel].name;
            for (int i = 0; nm[i] && i < 30; i++) b[k++] = nm[i];
            k = sk_str(b, k, "'? Y/N");
            b[k] = 0;
        } else if (files_in == 5) {                      // подтверждение формата
            k = sk_str(b, k, "Format P");
            k = sk_num(b, k, (unsigned int)files_part);
            k = sk_str(b, k, "? All data lost! Y/N");
            b[k] = 0;
        } else if (files_msg[0]) {                       // сообщение операции
            k = sk_str(b, k, files_msg);
            b[k] = 0;
        } else {                                         // обычная подсказка
            k = sk_str(b, k, "L/R part  N new  D del  R ren  F fmt  Ent view");
            b[k] = 0;
        }
        draw_text(tx, fy + fh - 20, b, TEXT_COL);
    } else {
        // --- просмотр содержимого файла ---
        int rows = (fh - TITLE_H - 26) / 16;
        files_text_rows = (rows > 1) ? rows : 1;
        if (vline_n == 0) draw_text(tx, ty, "(empty file)", TEXT_COL);
        for (int r = 0; r < files_text_rows; r++) {
            int li = files_view_top + r;
            if (li >= vline_n) break;
            unsigned int off = vline_off[li];
            unsigned int end = (li + 1 < vline_n) ? vline_off[li + 1] : view_len;
            int c = 0;
            for (unsigned int p = off; p < end && c < cols; p++) {
                char ch = view_buf[p];
                if (ch == '\n') break;
                if (ch < 32) ch = '.';               // невидимые символы — точками
                draw_char(tx + c * 8, ty + r * 16, ch, TEXT_COL);
                c++;
            }
        }
        // внизу: номер первой строки / всего строк + подсказка
        char inf[32]; int ik = sk_str(inf, 0, "Line ");
        ik = sk_num(inf, ik, (unsigned int)(files_view_top + 1));
        ik = sk_str(inf, ik, "/");
        ik = sk_num(inf, ik, (unsigned int)vline_n);
        inf[ik] = 0;
        draw_text(tx, fy + fh - 20, inf, TEXT_COL);
        draw_text(tx + 120, fy + fh - 20, "Up/Dn scroll  Esc back", TEXT_COL);
    }
}

// Найти файл по имени в загруженном списке (чтобы выделить его после операции)
int files_find(const char* name) {
    for (int i = 0; i < files_n; i++)
        if (str_eq(file_list[i].name, name)) return i;
    return -1;
}

// Показать сообщение о результате операции (вместо нижней подсказки)
void fin_msg(const char* s) {
    int i = 0;
    while (s[i] && i < 55) { files_msg[i] = s[i]; i++; }
    files_msg[i] = 0;
    files_in = 0;
    draw_files();
}

// Подтверждение «Y»: удаление выбранного файла (4) или форматирование (5)
void files_confirm_yes() {
    if (files_in == 4) {
        char nm[56];
        const char* src = file_list[files_sel].name;
        int i = 0; while (src[i] && i < 55) { nm[i] = src[i]; i++; } nm[i] = 0;
        int r = fs_delete(files_part, nm);
        if (r == 0) {
            char m[56]; int k;
            k = sk_str(m, 0, "Deleted ");
            k = sk_str(m, k, nm);
            m[k] = 0;
            files_load();
            fin_msg(m);
        } else if (r == -3) fin_msg("No such file");
        else if (r == -1) fin_msg("No such partition");
        else fin_msg("Not MyFS");
        return;
    }
    if (files_in == 5) {
        int r = fs_format(files_part, true);
        if (r == 0) {
            files_load();
            files_sel = 0;
            char m[56]; int k;
            k = sk_str(m, 0, "Formatted P");
            k = sk_num(m, k, (unsigned int)files_part);
            m[k] = 0;
            fin_msg(m);
        } else fin_msg("No such partition");
        return;
    }
}

// Enter в режиме ввода: шаг имени → шаг текста → создание; для rename — сразу
void files_input_done() {
    fin_buf[fin_len] = 0;                        // буфер как C-строка
    if (files_in == 1) {                         // ввели имя нового файла
        if (fin_len == 0) { fin_msg("Name is empty"); return; }
        for (int i = 0; i <= fin_len; i++) fin_name[i] = fin_buf[i];
        for (int i = 0; i < files_n; i++)
            if (str_eq(file_list[i].name, fin_name)) { fin_msg("File exists"); return; }
        files_in = 2;                            // теперь текст файла
        fin_len = 0;
        draw_files();
        return;
    }
    if (files_in == 2) {                         // ввели текст — создаём файл
        int tlen = fin_len;
        int r = fs_write(files_part, fin_name, fin_buf, true);
        if (r == 0) {
            files_load();
            int ix = files_find(fin_name);
            if (ix >= 0) files_sel = ix;
            char m[56]; int k;
            k = sk_str(m, 0, "Created ");
            k = sk_str(m, k, fin_name);
            k = sk_str(m, k, ", ");
            k = sk_num(m, k, (unsigned int)tlen);
            k = sk_str(m, k, " bytes");
            m[k] = 0;
            fin_msg(m);
        } else if (r == -3) fin_msg("File exists");
        else if (r == -4) fin_msg("File table full");
        else if (r == -5) fin_msg("No space on partition");
        else if (r == -1) fin_msg("No such partition");
        else fin_msg("Not MyFS - press F to format");
        return;
    }
    if (files_in == 3) {                         // новое имя (переименование)
        if (fin_len == 0) { fin_msg("Name is empty"); return; }
        int r = fs_rename(files_part, file_list[files_sel].name, fin_buf);
        if (r == 0) {
            char m[56]; int k;
            k = sk_str(m, 0, "Renamed to ");
            k = sk_str(m, k, fin_buf);
            m[k] = 0;
            files_load();
            int ix = files_find(fin_buf);
            if (ix >= 0) files_sel = ix;
            fin_msg(m);
        } else if (r == -4) fin_msg("Name already exists");
        else if (r == -3) fin_msg("No such file");
        else if (r == -1) fin_msg("No such partition");
        else fin_msg("Not MyFS");
        return;
    }
}

// Клавиши в режиме ввода/подтверждения
void files_input_key(int sc) {
    if (files_in == 4 || files_in == 5) {        // ждём Y/N
        if (sc == 0x15) { files_confirm_yes(); return; }              // Y — да
        if (sc == 0x31 || sc == 0x01 || sc == SC_F3) {                // N/Esc — отмена
            files_in = 0;
            draw_files();
        }
        return;
    }
    if (sc == 0x01 || sc == SC_F3) { files_in = 0; draw_files(); return; }    // отмена
    if (sc == 0x1C) { files_input_done(); return; }                           // подтвердить
    if (sc == 0x0E) { if (fin_len > 0) { fin_len--; draw_files(); } return; } // Backspace
    if (sc == 0x2A || sc == 0x36) { shift_down = true; return; }              // Shift зажат
    if (sc >= 0x3A) return;                                                   // не символ
    char c = (char)sc_ascii[sc];
    if (!c) return;
    if (shift_down) c = apply_shift(c);
    int cap = (files_in == 2) ? 40 : 24;         // имя — как в консоли (24), текст — 40
    if (fin_len < cap) { fin_buf[fin_len++] = c; draw_files(); }
}

// Открыть проводник (из дока)
void files_open() {
    win_place(APP_FILES, win_w(400), win_h(300));
    win_raise(APP_FILES);
    files_active = true;
    files_sel = 0;
    files_in = 0;
    files_msg[0] = 0;
    files_load();
    draw_desktop();
}

// Клавиши в окне проводника
void files_handle_key(int sc) {
    if (sc == 0xE0) { e0_prefix = true; return; }
    bool ext = e0_prefix; e0_prefix = false;
    if (sc & 0x80) {                             // отпускание Shift — снять флаг
        if ((sc & 0x7F) == 0x2A || (sc & 0x7F) == 0x36) shift_down = false;
        return;
    }

    if (files_msg[0]) {                          // сообщение снимается, клавиша работает дальше
        files_msg[0] = 0;
        draw_files();
    }
    if (files_in != 0) {                         // режим ввода/подтверждения
        files_input_key(sc);
        return;
    }

    if (ext) {                                   // стрелки
        if (!files_view) {
            int cnt = (files_n > 0) ? files_n : 0;
            if (sc == 0x48 && files_sel > 0) { files_sel--; draw_files(); }               // Up
            if (sc == 0x50 && files_sel < cnt - 1) { files_sel++; draw_files(); }         // Down
            if (sc == 0x4B) {                    // Left — предыдущий раздел
                files_part--; if (files_part < 1) files_part = 4;
                files_load(); draw_files();
            }
            if (sc == 0x4D) {                    // Right — следующий раздел
                files_part++; if (files_part > 4) files_part = 1;
                files_load(); draw_files();
            }
        } else {
            int max_top = vline_n - files_text_rows;   // чтобы последняя строка была видна
            if (max_top < 0) max_top = 0;
            if (sc == 0x48 && files_view_top > 0) { files_view_top--; draw_files(); }     // Up
            if (sc == 0x50 && files_view_top < max_top) { files_view_top++; draw_files(); } // Down
        }
        return;
    }
    if (sc == 0x01 || sc == SC_F3) {             // Esc: из просмотра — назад, из списка — закрыть
        if (files_view) { files_view = false; draw_files(); }
        else { files_active = false; win_close_app(APP_FILES); draw_desktop(); }
        return;
    }
    if (sc == 0x1C) {                            // Enter
        if (files_n > 0 && !files_view) {        // открыть выбранный файл
            fs_read_file(file_list[files_sel].start, file_list[files_sel].size);
            files_view = true;
            files_view_top = 0;
            files_wrap((fw - 20) / 8);           // ширина окна известна с прошлой отрисовки
            draw_files();
        } else if (files_view) {                 // из просмотра — назад к списку
            files_view = false;
            draw_files();
        }
        return;
    }
    // горячие клавиши операций (только в списке)
    if (files_view) return;
    if (sc == 0x31) {                            // N — новый файл
        if (files_n >= 0) { files_in = 1; fin_len = 0; draw_files(); }
        else if (files_n == -1) fin_msg("No such partition");
        else fin_msg("Not MyFS - press F to format");
        return;
    }
    if (sc == 0x20) {                            // D — удалить
        if (files_n > 0) { files_in = 4; draw_files(); }
        else if (files_n == 0) fin_msg("Nothing to delete");
        else if (files_n == -1) fin_msg("No such partition");
        else fin_msg("Not MyFS - press F to format");
        return;
    }
    if (sc == 0x13) {                            // R — переименовать
        if (files_n > 0) { files_in = 3; fin_len = 0; draw_files(); }
        else if (files_n == 0) fin_msg("Nothing to rename");
        else if (files_n == -1) fin_msg("No such partition");
        else fin_msg("Not MyFS - press F to format");
        return;
    }
    if (sc == 0x21) {                            // F — форматировать раздел
        if (files_n != -1) { files_in = 5; draw_files(); }
        else fin_msg("No such partition");
    }
}

// --- НАСТРОЙКИ (Settings) ---
bool set_active = false;             // открыто ли окно
int  set_sel = 0;                    // выбранная строка
const int SET_ROWS = 4;              // строка часов + три строки-справки

// Нарисовать окно настроек
void draw_settings() {
    int sw = win_w(380), sh = win_h(230);
    win_wp[APP_SET] = sw; win_hp[APP_SET] = sh;
    int sx = win_x[APP_SET], sy = win_y[APP_SET];
    draw_window_frame(sx, sy, sw, sh, "Settings");
    int tx = sx + 10, ty = sy + TITLE_H + 10;

    for (int i = 0; i < SET_ROWS; i++) {
        int y = ty + i * 22;
        unsigned int col = TEXT_COL;
        if (i == set_sel) {                       // выбранная строка — инверсия
            fill_rect(tx - 2, y - 2, sw - 16, 16, C_WHITE);
            col = C_BLACK;
        }
        char b[48]; int k = 0;
        if (i == 0) {
            k = sk_str(b, k, "Clock: ");
            k = sk_str(b, k, clock_24h ? "24-hour" : "12-hour (AM/PM)");
        } else if (i == 1) {
            k = sk_str(b, k, "Video: ");
            k = sk_num(b, k, (unsigned int)W); k = sk_str(b, k, "x");
            k = sk_num(b, k, (unsigned int)H); k = sk_str(b, k, "x");
            k = sk_num(b, k, (unsigned int)FB_BPP);
        } else if (i == 2) {
            k = sk_str(b, k, "Kernel: ");
            k = sk_num(b, k, (unsigned int)(KERNEL_SECTORS * 512 / 1024));
            k = sk_str(b, k, " KB / boot area ");
            k = sk_num(b, k, PART_RESERVED);
            k = sk_str(b, k, " sec");
        } else {
            k = sk_str(b, k, "Boot: ");
            unsigned short lba = 0;
            int p = boot_partition(&lba);
            if (p > 0) {
                k = sk_str(b, k, "P"); k = sk_num(b, k, (unsigned int)p);
                k = sk_str(b, k, " (lba="); k = sk_num(b, k, lba);
                k = sk_str(b, k, ")");
            } else {
                k = sk_str(b, k, (p == 0) ? "? (not in table)" : "(raw boot)");
            }
        }
        b[k] = 0;
        draw_text(tx, y, b, col);
    }
    draw_text(tx, sy + sh - 20, "Up/Dn select  Enter/L/R change", TEXT_COL);
}

// Открыть настройки (из дока)
void set_open() {
    win_place(APP_SET, win_w(380), win_h(230));
    win_raise(APP_SET);
    set_active = true;
    set_sel = 0;
    draw_desktop();
}

// Клавиши в окне настроек
void set_handle_key(int sc) {
    if (sc == 0xE0) { e0_prefix = true; return; }
    bool ext = e0_prefix; e0_prefix = false;
    if (sc & 0x80) return;

    if (ext) {
        if (sc == 0x48 && set_sel > 0) { set_sel--; draw_settings(); }                // Up
        if (sc == 0x50 && set_sel < SET_ROWS - 1) { set_sel++; draw_settings(); }     // Down
        if ((sc == 0x4B || sc == 0x4D) && set_sel == 0) {                             // L/R — сменить
            clock_24h = !clock_24h;
            draw_settings();
            draw_clock();                       // сразу видно в меню-баре
        }
        return;
    }
    if (sc == 0x01 || sc == SC_F3) { set_active = false; win_close_app(APP_SET); draw_desktop(); return; }    // Esc/F3
    if (sc == 0x1C && set_sel == 0) {           // Enter — сменить формат часов
        clock_24h = !clock_24h;
        draw_settings();
        draw_clock();
    }
}

// --- ДИСПЕТЧЕР ЗАДАЧ (Task Manager) ---
bool tasks_active = false;           // открыто ли окно
int  boot_secs = 0;                  // время запуска системы (секунды суток) — для Uptime
int  tasks_last_sec = -1;            // секунда последнего обновления
int  tkx, tky, tkw, tkh;             // геометрия окна (для точечного обновления)

// Текущее время в секундах суток (из часов CMOS)
int day_secs() {
    return bcd2bin(cmos_read(4)) * 3600 + bcd2bin(cmos_read(2)) * 60 + bcd2bin(cmos_read(0));
}

// Строка «Uptime: 0:12:34» — обновляется отдельно, не перерисовывая всё окно
void tasks_draw_uptime() {
    int up = day_secs() - boot_secs;
    if (up < 0) up += 24 * 3600;                 // пережили полночь
    char b[32]; int k = sk_str(b, 0, "Uptime: ");
    k = sk_num(b, k, (unsigned int)(up / 3600)); k = sk_str(b, k, ":");
    int mm = (up % 3600) / 60, ss = up % 60;
    if (mm < 10) b[k++] = '0';
    k = sk_num(b, k, (unsigned int)mm); k = sk_str(b, k, ":");
    if (ss < 10) b[k++] = '0';
    k = sk_num(b, k, (unsigned int)ss);
    b[k] = 0;
    int x = tkx + 10, y = tky + TITLE_H + 8;
    fill_rect(x - 2, y - 1, 150, 16, C_BLACK);              // стереть старую строку
    draw_text(x, y, b, TEXT_COL);
}

// Нарисовать окно диспетчера: система + список компонентов
void draw_tasks() {
    tkw = win_w(400); tkh = win_h(300);
    win_wp[APP_TASKS] = tkw; win_hp[APP_TASKS] = tkh;
    tkx = win_x[APP_TASKS]; tky = win_y[APP_TASKS];
    draw_window_frame(tkx, tky, tkw, tkh, "Task Manager");
    int tx = tkx + 10, ty = tky + TITLE_H + 8;
    char b[64]; int k;

    tasks_draw_uptime();                         // первая строка — «Uptime: ...»
    ty += 20;

    k = sk_str(b, 0, "Video: ");
    k = sk_num(b, k, (unsigned int)W); k = sk_str(b, k, "x");
    k = sk_num(b, k, (unsigned int)H); k = sk_str(b, k, "x");
    k = sk_num(b, k, (unsigned int)FB_BPP); k = sk_str(b, k, " @ 0x");
    unsigned int a = (unsigned int)fb;
    k = sk_hex8(b, k, (unsigned char)(a >> 24));
    k = sk_hex8(b, k, (unsigned char)(a >> 16));
    k = sk_hex8(b, k, (unsigned char)(a >> 8));
    k = sk_hex8(b, k, (unsigned char)a);
    b[k] = 0;
    draw_text(tx, ty, b, TEXT_COL);
    ty += 20;

    k = sk_str(b, 0, "Kernel ");
    k = sk_num(b, k, (unsigned int)(KERNEL_SECTORS * 512 / 1024));
    k = sk_str(b, k, " KB, boot area ");
    k = sk_num(b, k, PART_RESERVED); k = sk_str(b, k, " sec");
    b[k] = 0;
    draw_text(tx, ty, b, TEXT_COL);
    ty += 20;

    unsigned short lba = 0;
    int p = boot_partition(&lba);
    k = sk_str(b, 0, "Boot: ");
    if (p > 0) {
        k = sk_str(b, k, "P"); k = sk_num(b, k, (unsigned int)p);
        k = sk_str(b, k, " (lba="); k = sk_num(b, k, lba); k = sk_str(b, k, ")");
    } else {
        k = sk_str(b, k, (p == 0) ? "? (not in table)" : "(raw boot)");
    }
    b[k] = 0;
    draw_text(tx, ty, b, TEXT_COL);
    ty += 28;

    // «Задачи»: компоненты системы и что сейчас работает
    draw_text(tx, ty, "Components:", TEXT_COL);
    ty += 18;
    for (int i = 0; i < 8; i++) {
        const char* nm;
        const char* st;
        if      (i == 0) { nm = "Desktop";  st = "active"; }
        else if (i == 1) { nm = "Menubar";  st = "active"; }
        else if (i == 2) { nm = "Dock";     st = "active"; }
        else if (i == 3) { nm = "CMD";      st = cmd_active   ? "running" : "idle"; }
        else if (i == 4) { nm = "Files";    st = files_active ? "running" : "idle"; }
        else if (i == 5) { nm = "Settings"; st = set_active   ? "running" : "idle"; }
        else if (i == 6) { nm = "Tasks";    st = "running"; }
        else             { nm = "About";    st = about_active ? "running" : "idle"; }
        int y = ty + i * 16;
        draw_text(tx, y, nm, TEXT_COL);
        draw_text(tx + 110, y, st, TEXT_COL);
    }
    draw_text(tx, tky + tkh - 20, "Esc closes this window", TEXT_COL);
}

// Открыть диспетчер (из дока)
void tasks_open() {
    win_place(APP_TASKS, win_w(400), win_h(300));
    win_raise(APP_TASKS);
    tasks_active = true;
    tasks_last_sec = bcd2bin(cmos_read(0));
    draw_desktop();
}

// Клавиши в окне диспетчера (он только «смотрит» — закрывается Esc/Enter/F3)
void tasks_handle_key(int sc) {
    if (sc == 0xE0) { e0_prefix = true; return; }
    bool ext = e0_prefix; e0_prefix = false;
    if (sc & 0x80) return;
    if (!ext && (sc == 0x01 || sc == 0x1C || sc == SC_F3)) {
        tasks_active = false;
        win_close_app(APP_TASKS);
        draw_desktop();
    }
}

// Раз в секунду обновляем «Uptime», пока окно диспетчера открыто
void tasks_tick() {
    if (!tasks_active) return;
    int s = bcd2bin(cmos_read(0));
    if (s == tasks_last_sec) return;             // та же секунда — не трогаем экран
    tasks_last_sec = s;
    cursor_hide();
    tasks_draw_uptime();
    cursor_show();
}

// ===========================================================================
//  КЛАВИШИ НА РАБОЧЕМ СТОЛЕ И В ОКНАХ ПРИЛОЖЕНИЙ
// ===========================================================================
//  ТАЙМЕР (PIT) — миллисекунды без прерываний
// ===========================================================================
//  Канал 0 системного таймера считает вниз с частотой 1.193182 МГц. Мы его
//  только читаем (защёлкиваем значение) и накапливаем пройденные «тики» — так
//  получаем время в миллисекундах, не включая прерывания (IDT у нас нет).
unsigned short pit_last = 0;
unsigned int   pit_acc  = 0;         // накопленные тики PIT (один тик ≈ 0.838 мкс)

void pit_init() {
    outb(0x43, 0x00);                // команда «защёлкнуть канал 0»
    unsigned char lo = inb(0x40);
    unsigned char hi = inb(0x40);
    pit_last = (unsigned short)(lo | (hi << 8));
}

// Обновить счётчик — вызывать часто в простое. Возврат: мс с момента pit_init
unsigned int millis() {
    outb(0x43, 0x00);
    unsigned char lo = inb(0x40);
    unsigned char hi = inb(0x40);
    unsigned short now = (unsigned short)(lo | (hi << 8));
    pit_acc += (unsigned int)(unsigned short)(pit_last - now);   // счёт идёт вниз
    pit_last = now;
    return pit_acc / 1193;           // 1193 тика PIT ≈ 1 мс
}

// ===========================================================================
//  ЗАСТАВКА ПРИ ЗАГРУЗКЕ — логотип и «полоса загрузки»
// ===========================================================================
// Один символ шрифта, увеличенный в scale раз (для крупного логотипа)
void draw_char_big(int x, int y, char c, int scale, unsigned int color) {
    if (c < 32 || c > 126) return;
    const unsigned char* glyph = font8x16[c - 32];
    for (int row = 0; row < 16; row++) {
        unsigned char bits = glyph[row];
        for (int col = 0; col < 8; col++)
            if (bits & (0x80 >> col))
                fill_rect(x + col * scale, y + row * scale, scale, scale, color);
    }
}
void draw_text_big(int x, int y, const char* s, int scale, unsigned int color) {
    while (*s) { draw_char_big(x, y, *s, scale, color); x += 8 * scale; s++; }
}

// Заставка: крупный логотип «MyOS», подпись и полоса загрузки (~1.4 с).
// Любая клавиша пропускает заставку.
void draw_splash() {
    fill_rect(0, 0, W, H, C_BLACK);
    const int scale = 5;                                 // 40x80 пикселей на символ
    const char* logo = "MyOS";
    int lw = 4 * 8 * scale;
    int lx = (W - lw) / 2, ly = H / 2 - 100;
    draw_text_big(lx, ly, logo, scale, TEXT_COL);        // логотип
    fill_rect(lx, ly + 16 * scale + 8, lw, 3, TEXT_COL); // линия под ним
    const char* sub = "mini operating system";
    draw_text((W - 21 * 8) / 2, ly + 16 * scale + 24, sub, TEXT_COL);
    // полоса загрузки
    int bw = 320, bh = 14;
    int bx = (W - bw) / 2, by = ly + 16 * scale + 64;
    draw_rect(bx, by, bw, bh, TEXT_COL);
    draw_text((W - 18 * 8) / 2, by + 28, "Loading kernel...", TEXT_COL);
    unsigned int t0 = millis();
    int shown = -1;
    for (;;) {
        unsigned int dt = millis() - t0;
        if (dt >= 1400) break;
        int w = (int)((unsigned long)dt * (unsigned long)(bw - 6) / 1400);
        if (w != shown) {                                // перерисуем только при изменении
            shown = w;
            fill_rect(bx + 3, by + 3, bw - 6, bh - 6, C_BLACK);
            fill_rect(bx + 3, by + 3, w, bh - 6, TEXT_COL);
        }
        if (poll_key() >= 0) break;                      // клавиша — пропустить заставку
    }
    fill_rect(0, 0, W, H, C_BLACK);                      // очистка перед рабочим столом
}

// ===========================================================================
//  МИНИ-ИГРА «ЗМЕЙКА» — из дока (Game) или командой play
// ===========================================================================
bool game_active = false;
const int G_CELL = 14;                     // размер клетки поля, пикселей
const int G_MAX  = 300;                    // предел длины змейки
unsigned char gsx[G_MAX], gsy[G_MAX];      // тело: элемент [0] — голова
int  gslen = 0;
int  gsdir = 1, gsnext = 1;                // направление: 0 вверх 1 вправо 2 вниз 3 влево
int  gfood_x = 0, gfood_y = 0;             // клетка с едой
int  gscore = 0;
bool gdead = false;
unsigned int gmove_at = 0;                 // время следующего шага (мс)
unsigned int grnd = 1;                     // состояние генератора случайных чисел

unsigned int g_rand() { grnd = grnd * 1103515245u + 12345u; return grnd >> 16; }

// Геометрия окна и поля (заполняется при открытии и отрисовке)
int gwx, gwy, gww, gwh, gfx, gfy, gcols, grows;

void game_layout() {
    gww = win_w(460); gwh = win_h(380);
    win_wp[APP_GAME] = gww; win_hp[APP_GAME] = gwh;
    gwx = win_x[APP_GAME]; gwy = win_y[APP_GAME];
    gfx = gwx + 8;  gfy = gwy + TITLE_H + 26;              // поле — под строкой счёта
    gcols = (gww - 16) / G_CELL; if (gcols > 32) gcols = 32;
    grows = (gwh - TITLE_H - 26 - 26) / G_CELL; if (grows > 24) grows = 24;
}

// Положить еду в свободную клетку поля
void game_food() {
    for (int tries = 0; tries < 500; tries++) {
        int x = (int)(g_rand() % (unsigned int)gcols);
        int y = (int)(g_rand() % (unsigned int)grows);
        bool taken = false;
        for (int i = 0; i < gslen; i++)
            if (gsx[i] == x && gsy[i] == y) { taken = true; break; }
        if (!taken) { gfood_x = x; gfood_y = y; return; }
    }
}

// Начать игру заново
void game_reset() {
    gslen = 4;
    for (int i = 0; i < gslen; i++) {                      // змейка в центре, «хвост» влево
        gsx[i] = (unsigned char)(gcols / 2 - i);
        gsy[i] = (unsigned char)(grows / 2);
    }
    gsdir = gsnext = 1;
    gscore = 0; gdead = false;
    grnd = millis() * 2654435761u + 12345u;                // сеем случайность временем
    if (grnd == 0) grnd = 7;
    game_food();
    gmove_at = millis();
}

void draw_game();                        // (определение ниже — объявим заранее)

// Открыть игру (из дока или команды play)
void game_open() {
    win_place(APP_GAME, win_w(460), win_h(380));
    win_raise(APP_GAME);
    game_active = true;
    game_layout();
    game_reset();
    draw_desktop();
}

// Нарисовать окно игры целиком
void draw_game() {
    game_layout();
    draw_window_frame(gwx, gwy, gww, gwh, "Snake");
    int tx = gwx + 10, ty = gwy + TITLE_H + 6;
    char sc[48]; int k = sk_str(sc, 0, "Score: "); k = sk_num(sc, k, (unsigned int)gscore);
    k = sk_str(sc, k, "   Length: "); k = sk_num(sc, k, (unsigned int)gslen);
    sc[k] = 0;
    draw_text(tx, ty, sc, TEXT_COL);
    fill_rect(gfx, gfy, gcols * G_CELL, grows * G_CELL, C_BLACK);   // чистое поле
    draw_rect(gfx - 1, gfy - 1, gcols * G_CELL + 2, grows * G_CELL + 2, TEXT_COL);
    fill_rect(gfx + gfood_x * G_CELL + 3, gfy + gfood_y * G_CELL + 3,   // еда
              G_CELL - 6, G_CELL - 6, TEXT_COL);
    for (int i = 0; i < gslen; i++) {                      // змейка: голова сплошная, тело рамкой
        int px = gfx + gsx[i] * G_CELL, py = gfy + gsy[i] * G_CELL;
        if (i == 0) fill_rect(px + 1, py + 1, G_CELL - 2, G_CELL - 2, TEXT_COL);
        else        draw_rect(px + 2, py + 2, G_CELL - 4, G_CELL - 4, TEXT_COL);
    }
    if (gdead) {                                           // конец игры — табличка
        int pw = 324, ph = 42;
        int px = gwx + (gww - pw) / 2, py = gwy + TITLE_H + (gwh - TITLE_H - ph) / 2;
        fill_rect(px, py, pw, ph, C_BLACK);
        draw_rect(px, py, pw, ph, TEXT_COL);
        draw_text(px + (pw - 9 * 8) / 2, py + 4, "GAME OVER", TEXT_COL);
        draw_text(px + (pw - 28 * 8) / 2, py + 22, "Enter - restart  Esc - exit", TEXT_COL);
    } else {
        draw_text(tx, gwy + gwh - 20, "Arrows - move   Esc - exit", TEXT_COL);
    }
}

// Перерисовать поле игры, не забыв про курсор (он может висеть над окном)
void game_redraw() { cursor_hide(); draw_desktop(); cursor_show(); }

// Шаг игры (вызывается из главного цикла, когда нажатий нет)
void game_tick() {
    if (!game_active || gdead) return;
    unsigned int now = millis();
    unsigned int step = 140;                               // базовый «такт», мс
    unsigned int fast = (unsigned int)gscore * 4;          // с ростом очков — быстрее
    if (fast > 80) fast = 80;
    step -= fast;
    if (now - gmove_at < step) return;
    gmove_at = now;
    gsdir = gsnext;
    int dx = (gsdir == 1) - (gsdir == 3);
    int dy = (gsdir == 2) - (gsdir == 0);
    int hx = gsx[0] + dx, hy = gsy[0] + dy;
    if (hx < 0 || hy < 0 || hx >= gcols || hy >= grows) { gdead = true; game_redraw(); return; }
    bool eat = (hx == gfood_x && hy == gfood_y);
    int limit = eat ? gslen : gslen - 1;                   // хвост «уходит» — в него можно
    for (int i = 0; i < limit; i++)
        if (gsx[i] == hx && gsy[i] == hy) { gdead = true; game_redraw(); return; }
    if (eat) { if (gslen < G_MAX) gslen++; gscore++; game_food(); }
    for (int i = gslen - 1; i > 0; i--) { gsx[i] = gsx[i - 1]; gsy[i] = gsy[i - 1]; }
    gsx[0] = (unsigned char)hx; gsy[0] = (unsigned char)hy;
    game_redraw();
}

void game_handle_key(int sc) {
    if (sc == 0xE0) { e0_prefix = true; return; }
    bool ext = e0_prefix; e0_prefix = false;
    if (sc & 0x80) return;
    if (sc == 0x01) { game_active = false; win_close_app(APP_GAME); draw_desktop(); return; }   // Esc — выход
    if (sc == 0x1C) {                                                  // Enter — заново
        if (gdead) { game_reset(); draw_game(); }
        return;
    }
    if (!ext || gdead) return;
    if (sc == 0x48 && gsdir != 2) gsnext = 0;    // Up
    if (sc == 0x50 && gsdir != 0) gsnext = 2;    // Down
    if (sc == 0x4B && gsdir != 1) gsnext = 3;    // Left
    if (sc == 0x4D && gsdir != 3) gsnext = 1;    // Right
}

// ===========================================================================
//  РЕДАКТОР КОДА MyC — из дока (Code) или командой code
// ===========================================================================
//  Сверху — редактор C-подобного кода. Снизу — панель «куда что легло»:
//  адреса буферов в ОЗУ, файл на диске (MyFS), байты машинного кода и
//  регистры CPU после запуска. F5 — скомпилировать в настоящий x86-код,
//  F6 — запустить его. Язык MyC: int, присваивания, + - * /, скобки, return.

bool code_active = false;
int  code_part = 1;                     // раздел MyFS для сохранения/открытия

char edit_buf[2048];                    // исходник (строки через '\n')
int  edit_len = 0;
int  edit_cur = 0;                      // позиция курсора в буфере
int  edit_top = 0;                      // первая видимая строка
char code_msg[64];                      // сообщение внизу окна
int  code_in = 0;                       // 0 нет, 1 сохранение, 2 открытие
char cin_buf[32]; int cin_len = 0;      // имя файла в диалоге
int  cdx = 0, cdy = 0;                  // где нарисован курсор (для мигания)
unsigned int code_blink = 0;            // последняя «половина секунды» мигания

// куда сохранили / откуда открыли (для строки «диск» в панели)
bool code_disk_ok = false;
int  code_disk_part = 0;
unsigned int code_disk_lba = 0, code_disk_sec = 0, code_disk_size = 0;
char code_disk_name[24];

void code_msg_set(const char* s) {
    int i = 0; while (s[i] && i < 63) { code_msg[i] = s[i]; i++; } code_msg[i] = 0;
}

// --- Разбор буфера на строки (пересчитывается при отрисовке) ---
int lstart[160]; int lcount = 0;
void code_lines() {
    lcount = 0; lstart[0] = 0; lcount = 1;
    for (int i = 0; i < edit_len && lcount < 159; i++)
        if (edit_buf[i] == '\n') lstart[lcount++] = i + 1;
}
void code_cursor_line(int* L, int* C) {
    int l = 0;
    for (int i = 0; i < lcount; i++) { if (lstart[i] <= edit_cur) l = i; else break; }
    *L = l; *C = edit_cur - lstart[l];
}

// --- Правка текста ---
void edit_insert(char c) {
    if (edit_len >= 2047) return;
    for (int i = edit_len; i > edit_cur; i--) edit_buf[i] = edit_buf[i - 1];
    edit_buf[edit_cur++] = c;
    edit_len++;
}
void edit_backspace() {
    if (edit_cur <= 0) return;
    for (int i = edit_cur - 1; i < edit_len - 1; i++) edit_buf[i] = edit_buf[i + 1];
    edit_cur--; edit_len--;
}
void code_move_v(int dir) {             // курсор на строку вверх/вниз (столбец сохраняем)
    code_lines();
    int L, C; code_cursor_line(&L, &C);
    int nL = L + dir;
    if (nL < 0 || nL >= lcount) return;
    int s = lstart[nL];
    int e = (nL + 1 < lcount) ? lstart[nL + 1] - 1 : edit_len;
    int len = e - s;
    edit_cur = s + ((C < len) ? C : len);
}
void code_home() { code_lines(); int L, C; code_cursor_line(&L, &C); edit_cur -= C; }
void code_end() {
    code_lines(); int L, C; code_cursor_line(&L, &C);
    edit_cur = (L + 1 < lcount) ? lstart[L + 1] - 1 : edit_len;
}

// ===========================================================================
//  МИНИ-КОМПИЛЯТОР: MyC -> настоящие байты x86
// ===========================================================================
//  Значения переменных лежат в mcv_value (массив ОЗУ), машинный код — в code_buf.
//  Компилятор кладёт код в code_buf абсолютными адресами, потом ядро ВЫЗЫВАЕТ
//  этот буфер как функцию (call) — код исполняется по-настоящему.
unsigned char code_buf[1536];
int  code_len = 0;
int  mcv_count = 0;                     // сколько переменных объявлено
char mcv_name[8][16];                   // их имена (для показа)
int  mcv_value[8];                      // их значения — вот эти ячейки ОЗУ видит код
unsigned int mc_regs[7];                // eax, ebx, ecx, edx, esp, ebp, eflags
int  mc_retval = 0;                     // значение из return
bool mc_ok = false;                     // последняя компиляция успешна
bool mc_ran = false;                    // программа уже запускалась
bool mc_bad = false;                    // ошибка компиляции
char mc_err[40]; int mc_err_line = 0;   // текст и строка ошибки
int  mc_line = 1;                       // текущая строка при разборе
const char* mc_src;                     // указатель разбора по исходнику
int  mc_guard[8];                       // счётчики итераций циклов (защита от вечного цикла)
int  mc_loop_depth = 0;                 // текущая вложенность циклов
int  mc_ret_jumps[16]; int mc_ret_count = 0;   // откуда прыгать в эпилог (все return)

// токен: 0 конец, 1 число, 2 имя, 3 символ
// для символов: mt_sym (первый) + mt_sym2 (второй, для ==, !=, <=, >=)
int mt_type = 0; unsigned int mt_num = 0; char mt_name[16];
char mt_sym = 0; char mt_sym2 = 0;

// проверки токена: «это одиночный символ c?» / «это пара c1c2?»
int mt_is(char c)  { return mt_type == 3 && mt_sym == c && mt_sym2 == 0; }
int mt_is2(char a, char b) { return mt_type == 3 && mt_sym == a && mt_sym2 == b; }

void mc_fail(const char* msg) {
    if (mc_bad) return;
    mc_bad = true;
    int i = 0; while (msg[i] && i < 39) { mc_err[i] = msg[i]; i++; } mc_err[i] = 0;
    mc_err_line = mc_line;
}
int mc_ident_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

// Следующий токен (пробелы и переводы строк пропускаем)
void mc_next() {
    for (;;) {
        while (*mc_src == ' ' || *mc_src == '\t' || *mc_src == '\r') mc_src++;
        char c = *mc_src;
        if (c == '\n') { mc_line++; mc_src++; continue; }
        if (c == 0) { mt_type = 0; return; }
        if (c >= '0' && c <= '9') {
            unsigned int v = 0;
            while (*mc_src >= '0' && *mc_src <= '9') { v = v * 10 + (unsigned)(*mc_src - '0'); mc_src++; }
            mt_type = 1; mt_num = v; return;
        }
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_') {
            int i = 0;
            while (mc_ident_char(*mc_src) && i < 15) mt_name[i++] = *mc_src++;
            mt_name[i] = 0; mt_type = 2; return;
        }
        if (c == '+' || c == '-' || c == '*' || c == '/' ||
            c == '(' || c == ')' || c == ';' || c == '{' || c == '}') {
            mt_sym = c; mt_sym2 = 0; mc_src++; mt_type = 3; return;
        }
        if (c == '=' || c == '!' || c == '<' || c == '>') {   // может быть 2 символа
            mt_sym = c; mt_sym2 = 0;
            if (mc_src[1] == '=') { mt_sym2 = '='; mc_src += 2; }  // == != <= >=
            else mc_src++;
            mt_type = 3; return;
        }
        mc_src++;                                 // чужой символ — пропускаем
    }
}

int mcv_find(const char* n) {
    for (int i = 0; i < mcv_count; i++) if (str_eq(mcv_name[i], n)) return i;
    return -1;
}

// --- Генератор байтов ---
void em1(unsigned char b) {
    if (code_len >= (int)sizeof(code_buf)) { mc_fail("program too long"); return; }
    code_buf[code_len++] = b;
}
void em4(unsigned int v) { em1((unsigned char)v); em1((unsigned char)(v >> 8)); em1((unsigned char)(v >> 16)); em1((unsigned char)(v >> 24)); }
void em_mov_eax_imm(unsigned int v)  { em1(0xB8); em4(v); }               // mov eax, imm32
void em_load_eax(unsigned int a)     { em1(0xA1); em4(a); }               // mov eax, [a]
void em_store_eax(unsigned int a)    { em1(0xA3); em4(a); }               // mov [a], eax
void em_store_reg(unsigned int a, int reg) {                              // mov [a], <reg>
    if (reg == 0) { em1(0xA3); em4(a); return; }
    em1(0x89); em1((unsigned char)(0x05 | (reg << 3))); em4(a);
}
void em_push_eax()     { em1(0x50); }
void em_pop_eax()      { em1(0x58); }
void em_mov_ecx_eax()  { em1(0x89); em1(0xC1); }                          // mov ecx, eax
void em_add()          { em1(0x01); em1(0xC8); }                          // add eax, ecx
void em_sub()          { em1(0x29); em1(0xC8); }                          // sub eax, ecx
void em_imul()         { em1(0x0F); em1(0xAF); em1(0xC1); }               // imul eax, ecx
void em_div_guard()    { em1(0x85); em1(0xC9); em1(0x74); em1(0x03); }    // test ecx,ecx; jz +3
void em_cdq_idiv()     { em1(0x99); em1(0xF7); em1(0xF9); }               // cdq; idiv ecx
void em_zero_dword(unsigned int a) { em1(0xC7); em1(0x05); em4(a); em4(0); }
void em_ret()          { em1(0xC3); }
// --- Операции для условий и переходов ---
void em_test_eax()     { em1(0x85); em1(0xC0); }              // test eax, eax
void em_cmp_eax_ecx()  { em1(0x39); em1(0xC8); }              // cmp eax, ecx
void em_movzx_eax_al() { em1(0x0F); em1(0xB6); em1(0xC0); }   // movzx eax, al
void em_setcc(int op) {                                       // результат сравнения -> 0/1
    em1(0x0F);
    if (op == 1) em1(0x9C);                                   // setl  <
    else if (op == 2) em1(0x9F);                              // setg  >
    else if (op == 3) em1(0x9E);                              // setle <=
    else if (op == 4) em1(0x9D);                              // setge >=
    else if (op == 5) em1(0x94);                              // sete  ==
    else em1(0x95);                                           // setne !=
    em1(0xC0);
    em_movzx_eax_al();
}
// Прыжки: сначала пишем нулевое смещение, потом em_patch впишет настоящее
void em_jmp(int* p)  { em1(0xE9); *p = code_len; em4(0); }    // jmp rel32
void em_jz(int* p)   { em1(0x0F); em1(0x84); *p = code_len; em4(0); }  // jz (rax==0)
void em_jae(int* p)  { em1(0x0F); em1(0x83); *p = code_len; em4(0); }  // jae (>=, беззнак.)
void em_patch(int p, int target) {                            // вписать rel32 = target-(p+4)
    unsigned int rel = (unsigned int)(target - (p + 4));
    if (p >= 0 && p + 3 < (int)sizeof(code_buf)) {
        code_buf[p]     = (unsigned char)rel;
        code_buf[p + 1] = (unsigned char)(rel >> 8);
        code_buf[p + 2] = (unsigned char)(rel >> 16);
        code_buf[p + 3] = (unsigned char)(rel >> 24);
    }
}
void em_inc_mem(unsigned int a) { em1(0xFF); em1(0x05); em4(a); }               // inc dword [a]
void em_cmp_mem_imm(unsigned int a, unsigned int v) { em1(0x81); em1(0x3D); em4(a); em4(v); }

// --- Разбор выражений ---
//  expr = cmp;  cmp = addsub [ relop addsub ];  addsub = term{+-term};
//  term = factor{*/factor};  factor = число | имя | ( expr )
//  relop = < | > | <= | >= | == | !=     (результат сравнения — 0 или 1)
void mc_expr();
void mc_factor() {
    if (mc_bad) return;
    if (mt_type == 1) { em_mov_eax_imm(mt_num); mc_next(); return; }
    if (mt_type == 2) {
        int i = mcv_find(mt_name);
        if (i < 0) { mc_fail("unknown variable"); return; }
        em_load_eax((unsigned int)(unsigned long)&mcv_value[i]);
        mc_next(); return;
    }
    if (mt_is('(')) {
        mc_next(); mc_expr();
        if (mt_is(')')) mc_next();
        else mc_fail("expected )");
        return;
    }
    mc_fail("expected value");
}
void mc_term() {
    mc_factor();
    for (;;) {
        if (mc_bad) return;
        if (!(mt_is('*') || mt_is('/'))) return;
        char op = mt_sym; mc_next();
        em_push_eax();                            // левый операнд — в стек
        mc_factor();
        if (mc_bad) return;
        em_mov_ecx_eax();                         // правый операнд — в ecx
        em_pop_eax();                             // левый — обратно в eax
        if (op == '*') em_imul();
        else { em_div_guard(); em_cdq_idiv(); }   // деление с защитой от нуля
    }
}
void mc_addsub() {
    mc_term();
    for (;;) {
        if (mc_bad) return;
        if (!(mt_is('+') || mt_is('-'))) return;
        char op = mt_sym; mc_next();
        em_push_eax();
        mc_term();
        if (mc_bad) return;
        em_mov_ecx_eax();
        em_pop_eax();
        if (op == '+') em_add(); else em_sub();
    }
}
void mc_cmp() {                                   // addsub [ relop addsub ]
    mc_addsub();
    if (mc_bad) return;
    int op = 0;                                   // 1< 2> 3<= 4>= 5== 6!=
    if      (mt_is('<'))          op = 1;
    else if (mt_is('>'))          op = 2;
    else if (mt_is2('<', '='))    op = 3;
    else if (mt_is2('>', '='))    op = 4;
    else if (mt_is2('=', '='))    op = 5;
    else if (mt_is2('!', '='))    op = 6;
    if (!op) return;
    mc_next();
    em_push_eax();
    mc_addsub();
    if (mc_bad) return;
    em_mov_ecx_eax();                             // ecx = правый
    em_pop_eax();                                 // eax = левый
    em_cmp_eax_ecx();                             // флаги по (eax - ecx)
    em_setcc(op);                                 // eax = 0 или 1
}
void mc_expr() { mc_cmp(); }

// --- Разбор операторов ---
void mc_stmt();                                   // объявлен заранее (нужен для блока)

// Один оператор или блок в фигурных скобках { ... }
void mc_block_or_stmt() {
    if (mt_is('{')) {
        mc_next();
        while (!mc_bad && !mt_is('}') && mt_type != 0) mc_stmt();
        if (!mt_is('}')) { mc_fail("expected }"); return; }
        mc_next();
        return;
    }
    mc_stmt();
}

// if (условие) оператор [else оператор]
void mc_if() {
    if (!mt_is('(')) { mc_fail("expected ("); return; }
    mc_next(); mc_expr();
    if (mc_bad) return;
    if (!mt_is(')')) { mc_fail("expected )"); return; }
    mc_next();
    em_test_eax();
    int jfalse; em_jz(&jfalse);                   // условие == 0 -> на else/конец
    mc_block_or_stmt();
    if (mc_bad) return;
    if (mt_type == 2 && str_eq(mt_name, "else")) {
        mc_next();
        int jend; em_jmp(&jend);
        em_patch(jfalse, code_len);               // сюда — если условие ложно
        mc_block_or_stmt();
        if (mc_bad) return;
        em_patch(jend, code_len);                 // сюда — после ветки then
    } else {
        em_patch(jfalse, code_len);
    }
}

// while (условие) оператор — со счётчиком итераций (защита от вечного цикла)
void mc_while() {
    if (!mt_is('(')) { mc_fail("expected ("); return; }
    mc_next();
    if (mc_loop_depth >= 8) { mc_fail("loops too deep"); return; }
    int slot = mc_loop_depth++;                   // у каждого цикла свой счётчик
    em_zero_dword((unsigned int)(unsigned long)&mc_guard[slot]);
    int top = code_len;                           // начало цикла: отсюда и считаем условие
    mc_expr();                                    // условие ВНУТРИ цикла (должно пересчитываться!)
    if (mc_bad) return;
    if (!mt_is(')')) { mc_fail("expected )"); return; }
    mc_next();
    em_test_eax();
    int jout; em_jz(&jout);                       // условие == 0 -> выход
    mc_block_or_stmt();
    if (mc_bad) return;
    em_inc_mem((unsigned int)(unsigned long)&mc_guard[slot]);
    em_cmp_mem_imm((unsigned int)(unsigned long)&mc_guard[slot], 20000000u);
    int jguard; em_jae(&jguard);                  // слишком много итераций -> выход
    int jback; em_jmp(&jback);
    em_patch(jback, top);                         // назад к проверке условия
    em_patch(jout, code_len);                     // обычный выход
    em_patch(jguard, code_len);                   // выход по защите
    mc_loop_depth--;
}

void mc_stmt() {
    if (mc_bad) return;
    if (mt_is(';')) { mc_next(); return; }        // пустой оператор
    if (mt_is('{')) { mc_block_or_stmt(); return; }
    if (mt_type == 2 && str_eq(mt_name, "if"))    { mc_next(); mc_if();    return; }
    if (mt_type == 2 && str_eq(mt_name, "while")) { mc_next(); mc_while(); return; }
    if (mt_type == 2 && str_eq(mt_name, "return")) {
        mc_next(); mc_expr();
        if (mc_bad) return;
        em_store_eax((unsigned int)(unsigned long)&mc_retval);
        if (mt_is(';')) mc_next(); else { mc_fail("expected ;"); return; }
        if (mc_ret_count < 16) {                  // прыжок в эпилог (а не «сквозь» код)
            int p; em_jmp(&p); mc_ret_jumps[mc_ret_count++] = p;
        }
        return;
    }
    if (mt_type == 2 && str_eq(mt_name, "int")) {
        mc_next();
        if (mt_type != 2) { mc_fail("expected name"); return; }
        if (mcv_count >= 8) { mc_fail("too many variables"); return; }
        int idx = mcv_count;
        for (int i = 0; i < 16; i++) mcv_name[idx][i] = 0;
        int i = 0; while (mt_name[i] && i < 15) { mcv_name[idx][i] = mt_name[i]; i++; }
        mcv_count++;
        mc_next();
        if (mt_is('=')) {
            mc_next(); mc_expr();
            if (mc_bad) return;
            em_store_eax((unsigned int)(unsigned long)&mcv_value[idx]);
        } else {
            em_zero_dword((unsigned int)(unsigned long)&mcv_value[idx]);
        }
        if (mt_is(';')) mc_next(); else { mc_fail("expected ;"); return; }
        return;
    }
    if (mt_type == 2) {                            // присваивание: name = expr;
        int idx = mcv_find(mt_name);
        if (idx < 0) { mc_fail("unknown variable"); return; }
        mc_next();
        if (!mt_is('=')) { mc_fail("expected ="); return; }
        mc_next(); mc_expr();
        if (mc_bad) return;
        em_store_eax((unsigned int)(unsigned long)&mcv_value[idx]);
        if (mt_is(';')) mc_next(); else { mc_fail("expected ;"); return; }
        return;
    }
    mc_fail("expected statement");
}

// Скомпилировать edit_buf -> code_buf
void mc_compile() {
    code_len = 0; mcv_count = 0; mc_bad = false; mc_ok = false; mc_ran = false;
    mc_ret_count = 0; mc_loop_depth = 0;
    mc_line = 1; mc_err[0] = 0; mc_err_line = 0;
    edit_buf[edit_len] = 0;                        // исходник как C-строка
    mc_src = edit_buf;
    mc_next();
    while (!mc_bad && mt_type != 0) mc_stmt();
    if (mc_bad) return;
    // ЭПИЛОГ: сюда ведут все return; снять регистры в ОЗУ и вернуться в ядро
    int ep = code_len;
    for (int i = 0; i < mc_ret_count; i++) em_patch(mc_ret_jumps[i], ep);
    em_store_reg((unsigned int)(unsigned long)&mc_regs[0], 0);   // eax
    em_store_reg((unsigned int)(unsigned long)&mc_regs[1], 3);   // ebx
    em_store_reg((unsigned int)(unsigned long)&mc_regs[2], 1);   // ecx
    em_store_reg((unsigned int)(unsigned long)&mc_regs[3], 2);   // edx
    em_store_reg((unsigned int)(unsigned long)&mc_regs[4], 4);   // esp
    em_store_reg((unsigned int)(unsigned long)&mc_regs[5], 5);   // ebp
    em1(0x9C); em1(0x58);                                        // pushfd; pop eax
    em_store_eax((unsigned int)(unsigned long)&mc_regs[6]);      // eflags
    em_ret();
    mc_ok = true;
}

// Запустить скомпилированный код
void mc_run() {
    for (int i = 0; i < 8; i++) { mcv_value[i] = 0; mc_guard[i] = 0; }
    for (int i = 0; i < 7; i++) mc_regs[i] = 0;
    mc_retval = 0; mc_ran = false;
    if (!mc_ok) return;
    void (*fn)() = (void(*)())code_buf;            // указатель на наши байты
    fn();                                          // ВЫПОЛНЯЕМ сгенерированный код
    mc_ran = true;
}

// ===========================================================================
//  ОТРИСОВКА окна редактора
// ===========================================================================
int cww, cwh, cwx, cwy;

// Курсор: белый блок (или стереть блок и вернуть символ)
void code_draw_cursor(bool on) {
    char ch = 0;
    if (edit_cur < edit_len && edit_buf[edit_cur] != '\n') ch = edit_buf[edit_cur];
    if (ch < 32) ch = ch ? ' ' : 0;
    if (on) {
        fill_rect(cdx, cdy, 8, 16, C_WHITE);
        if (ch) draw_char(cdx, cdy, ch, C_BLACK);
    } else {
        fill_rect(cdx, cdy, 8, 16, C_BLACK);
        if (ch) draw_char(cdx, cdy, ch, TEXT_COL);
    }
}

// Мигание курсора (вызывается из главного цикла в простое)
void code_tick() {
    if (!code_active || code_in != 0) return;
    unsigned int t = millis();
    if (t / 500 == code_blink) return;
    code_blink = t / 500;
    code_draw_cursor((code_blink & 1) == 0);
}

// Панель «куда что легло»: адреса, диск, байты, регистры
void code_panel(int ytop, int ybot) {
    int tx = cwx + 10;
    int cols = (cww - 20) / 8;
    char b[200]; char row[200]; int k;
    int y = ytop;

    k = sk_str(b, 0, "CODE 0x"); k = sk_hex32(b, k, (unsigned int)(unsigned long)code_buf);
    k = sk_str(b, k, " "); k = sk_num(b, k, (unsigned int)code_len); k = sk_str(b, k, "B");
    k = sk_str(b, k, "    VARS 0x"); k = sk_hex32(b, k, (unsigned int)(unsigned long)mcv_value);
    k = sk_str(b, k, " x"); k = sk_num(b, k, (unsigned int)mcv_count);
    b[k] = 0; draw_text(tx, y, b, TEXT_COL); y += 14;

    k = sk_str(b, 0, "SRC  0x"); k = sk_hex32(b, k, (unsigned int)(unsigned long)edit_buf);
    k = sk_str(b, k, " "); k = sk_num(b, k, (unsigned int)edit_len); k = sk_str(b, k, "B");
    k = sk_str(b, k, "    REGS 0x"); k = sk_hex32(b, k, (unsigned int)(unsigned long)mc_regs);
    b[k] = 0; draw_text(tx, y, b, TEXT_COL); y += 14;

    k = 0;
    if (code_disk_ok) {
        k = sk_str(b, k, "DISK P"); k = sk_num(b, k, (unsigned int)code_disk_part);
        k = sk_str(b, k, " "); 
        for (int i = 0; i < 24 && code_disk_name[i]; i++) b[k++] = code_disk_name[i];
        k = sk_str(b, k, " @ LBA "); k = sk_num(b, k, code_disk_lba);
        k = sk_str(b, k, " x"); k = sk_num(b, k, code_disk_sec); k = sk_str(b, k, " sect");
    } else {
        k = sk_str(b, k, "DISK not saved yet");
    }
    b[k] = 0; draw_text(tx, y, b, TEXT_COL); y += 14;

    if (!mc_ok) {
        if (mc_bad) {
            k = sk_str(b, 0, "COMPILE ERROR line "); k = sk_num(b, k, (unsigned int)mc_err_line);
            k = sk_str(b, k, ": "); k = sk_str(b, k, mc_err);
        } else {
            k = sk_str(b, 0, "Not compiled yet - press F5");
        }
        b[k] = 0; draw_text(tx, y, b, TEXT_COL); y += 14;
    } else {
        int show = code_len; if (show > 60) show = 60;
        k = sk_str(b, 0, "BIN ");
        for (int i = 0; i < show && k < 190; i++) { k = sk_hex8(b, k, code_buf[i]); b[k++] = ' '; }
        if (show < code_len) k = sk_str(b, k, "...");
        b[k] = 0;
        int n = 0; while (b[n]) n++;
        for (int off = 0; off < n && y <= ybot; off += cols) {
            int m = 0;
            for (int i = off; i < n && i < off + cols; i++) row[m++] = b[i];
            row[m] = 0;
            draw_text(tx, y, row, TEXT_COL);
            y += 14;
        }
    }

    if (mc_ran) {
        k = sk_str(b, 0, "EAX "); k = sk_hex32(b, k, mc_regs[0]);
        k = sk_str(b, k, "  ECX "); k = sk_hex32(b, k, mc_regs[2]);
        k = sk_str(b, k, "  ESP "); k = sk_hex32(b, k, mc_regs[4]);
        b[k] = 0; draw_text(tx, y, b, TEXT_COL); y += 14;
        k = sk_str(b, 0, "EBX "); k = sk_hex32(b, k, mc_regs[1]);
        k = sk_str(b, k, "  EDX "); k = sk_hex32(b, k, mc_regs[3]);
        k = sk_str(b, k, "  EBP "); k = sk_hex32(b, k, mc_regs[5]);
        b[k] = 0; draw_text(tx, y, b, TEXT_COL); y += 14;
        k = sk_str(b, 0, "EFLAGS "); k = sk_hex32(b, k, mc_regs[6]);
        k = sk_str(b, k, "   RESULT = ");
        int rv = mc_retval;
        if (rv < 0) { b[k++] = '-'; rv = -rv; }
        k = sk_num(b, k, (unsigned int)rv);
        b[k] = 0; draw_text(tx, y, b, TEXT_COL); y += 14;
    } else if (mc_ok) {
        k = sk_str(b, 0, "Registers: press F6 to run and capture them");
        b[k] = 0; draw_text(tx, y, b, TEXT_COL); y += 14;
    }
}

void draw_code() {
    cww = win_w(680); cwh = win_h(500);
    win_wp[APP_CODE] = cww; win_hp[APP_CODE] = cwh;
    cwx = win_x[APP_CODE]; cwy = win_y[APP_CODE];
    char t[80]; int k = sk_str(t, 0, "Code - MyC  [P");
    k = sk_num(t, k, (unsigned int)code_part); k = sk_str(t, k, "]"); t[k] = 0;
    draw_window_frame(cwx, cwy, cww, cwh, t);

    int ed_y = cwy + TITLE_H + 6;
    int ed_rows = (cwh - TITLE_H - 60 - 130) / 16;
    if (ed_rows < 6) ed_rows = 6;
    if (ed_rows > 20) ed_rows = 20;
    int ed_h = ed_rows * 16;
    int pan_y = ed_y + ed_h + 10;

    code_lines();
    int gutter = 4 * 8;
    int cols = (cww - 20 - gutter) / 8;
    int cl, cc; code_cursor_line(&cl, &cc);
    if (cl < edit_top) edit_top = cl;
    if (cl >= edit_top + ed_rows) edit_top = cl - ed_rows + 1;
    if (edit_top < 0) edit_top = 0;

    code_blink = millis() / 500;                   // синхронизируем мигание
    cdx = -1000; cdy = -1000;
    for (int r = 0; r < ed_rows; r++) {
        int li = edit_top + r;
        if (li >= lcount) continue;
        int y = ed_y + r * 16;
        char nb[8]; int nk = sk_num(nb, 0, (unsigned int)(li + 1));    // номер строки
        draw_text(cwx + 8 + (3 - nk) * 8, y, nb, TEXT_COL);
        int s = lstart[li];
        int e = (li + 1 < lcount) ? lstart[li + 1] - 1 : edit_len;
        int col = 0;
        for (int i = s; i < e && col < cols; i++, col++) {
            char chh = edit_buf[i]; if (chh < 32) chh = ' ';
            draw_char(cwx + 8 + gutter + col * 8, y, chh, TEXT_COL);
        }
        if (li == cl && cc < cols) { cdx = cwx + 8 + gutter + cc * 8; cdy = y; }
    }
    if (cdx > -1000) code_draw_cursor(true);

    draw_line(cwx + 8, pan_y - 6, cwx + cww - 9, pan_y - 6, TEXT_COL);
    code_panel(pan_y, cwy + cwh - 24);

    // нижняя строка: подсказка, диалог или сообщение
    int by = cwy + cwh - 20;
    if (code_in == 1) {
        k = sk_str(t, 0, "Save as: ");
        for (int i = 0; i < cin_len; i++) t[k++] = cin_buf[i];
        t[k++] = '_'; t[k] = 0;
    } else if (code_in == 2) {
        k = sk_str(t, 0, "Open: ");
        for (int i = 0; i < cin_len; i++) t[k++] = cin_buf[i];
        t[k++] = '_'; t[k] = 0;
    } else if (code_msg[0]) {
        k = sk_str(t, 0, code_msg); t[k] = 0;
    } else {
        k = sk_str(t, 0, "F2 save  F4 open  F5 compile  F6 run  F7 part  Esc close");
        t[k] = 0;
    }
    draw_text(cwx + 10, by, t, TEXT_COL);
}

// ===========================================================================
//  КЛАВИШИ редактора кода
// ===========================================================================
void code_input_done() {
    cin_buf[cin_len] = 0;
    if (cin_len == 0) { code_in = 0; draw_code(); return; }
    if (code_in == 1) {                             // СОХРАНИТЬ в MyFS
        edit_buf[edit_len] = 0;
        int r = fs_write(code_part, cin_buf, edit_buf, true);
        if (r == 0) {
            int n = fs_load_table(code_part);
            for (int i = 0; i < n; i++)
                if (str_eq(file_list[i].name, cin_buf)) {
                    code_disk_ok = true; code_disk_part = code_part;
                    code_disk_lba = file_list[i].start; code_disk_size = file_list[i].size;
                    code_disk_sec = (file_list[i].size + 511) / 512;
                    for (int j = 0; j < 24; j++) code_disk_name[j] = 0;
                    int j = 0; while (cin_buf[j] && j < 23) { code_disk_name[j] = cin_buf[j]; j++; }
                    break;
                }
            code_msg_set("Saved to MyFS");
        } else if (r == -2) code_msg_set("Not MyFS - format the partition (Files: F)");
        else if (r == -1) code_msg_set("No such partition");
        else if (r == -3) code_msg_set("File exists");
        else if (r == -4) code_msg_set("File table full");
        else code_msg_set("No space on partition");
    } else {                                        // ОТКРЫТЬ из MyFS
        int n = fs_load_table(code_part);
        if (n == -1) code_msg_set("No such partition");
        else if (n == -2) code_msg_set("Not MyFS (format it first)");
        else {
            int ix = -1;
            for (int i = 0; i < n; i++) if (str_eq(file_list[i].name, cin_buf)) ix = i;
            if (ix < 0) code_msg_set("No such file");
            else {
                int len = fs_read_content(ix, edit_buf, 2047);
                edit_len = len; edit_cur = 0; edit_top = 0;
                mc_ok = false; mc_ran = false; mc_bad = false;
                code_disk_ok = true; code_disk_part = code_part;
                code_disk_lba = file_list[ix].start; code_disk_size = file_list[ix].size;
                code_disk_sec = (file_list[ix].size + 511) / 512;
                for (int j = 0; j < 24; j++) code_disk_name[j] = 0;
                int j = 0; while (cin_buf[j] && j < 23) { code_disk_name[j] = cin_buf[j]; j++; }
                code_msg_set("Opened from MyFS");
            }
        }
    }
    code_in = 0;
    draw_code();
}

void code_input_key(int sc) {
    if (sc == 0x01) { code_in = 0; draw_code(); return; }              // Esc — отмена
    if (sc == 0x0E) { if (cin_len > 0) { cin_len--; draw_code(); } return; }  // Backspace
    if (sc == 0x1C) { code_input_done(); return; }                     // Enter — готово
    if (sc == 0x2A || sc == 0x36) { shift_down = true; return; }       // Shift
    if (sc >= 0x3A) return;                                            // не символ
    char c = (char)sc_ascii[sc];
    if (!c) return;
    if (shift_down) c = apply_shift(c);
    if (cin_len < 24) { cin_buf[cin_len++] = c; draw_code(); }
}

void code_handle_key(int sc) {
    if (sc == 0xE0) { e0_prefix = true; return; }
    bool ext = e0_prefix; e0_prefix = false;
    if (sc & 0x80) {                                  // отпускание Shift — снять флаг
        if ((sc & 0x7F) == 0x2A || (sc & 0x7F) == 0x36) shift_down = false;
        return;
    }
    if (code_msg[0]) { code_msg[0] = 0; }             // сообщение снимаем, клавиша работает
    if (code_in != 0) { code_input_key(sc); return; }

    if (ext) {                                        // стрелки и Home/End
        if (sc == 0x4B) { if (edit_cur > 0) edit_cur--; draw_code(); }            // Left
        if (sc == 0x4D) { if (edit_cur < edit_len) edit_cur++; draw_code(); }     // Right
        if (sc == 0x48) { code_move_v(-1); draw_code(); }                        // Up
        if (sc == 0x50) { code_move_v(1); draw_code(); }                         // Down
        if (sc == 0x47) { code_home(); draw_code(); }                            // Home
        if (sc == 0x4F) { code_end(); draw_code(); }                             // End
        return;
    }
    if (sc == 0x01) { code_active = false; win_close_app(APP_CODE); draw_desktop(); return; }             // Esc — закрыть
    if (sc == 0x3C) { code_in = 1; cin_len = 0; draw_code(); return; }           // F2 — сохранить
    if (sc == 0x3E) { code_in = 2; cin_len = 0; draw_code(); return; }           // F4 — открыть
    if (sc == 0x3F) {                                                            // F5 — компилировать
        mc_compile();
        if (mc_ok) code_msg_set("Compiled - code bytes are ready (F6 to run)");
        else code_msg_set("Compile error - see the panel");
        draw_code();
        return;
    }
    if (sc == 0x40) {                                                            // F6 — запустить
        if (!mc_ok) code_msg_set("Compile first (F5)");
        else { mc_run(); code_msg_set("Program executed - see the registers"); }
        draw_code();
        return;
    }
    if (sc == 0x41) { code_part++; if (code_part > 4) code_part = 1; draw_code(); return; }  // F7 — раздел
    if (sc == 0x1C) {                                                            // Enter — новая строка
        edit_insert('\n');
        draw_code();
        return;
    }
    if (sc == 0x0E) { edit_backspace(); draw_code(); return; }                   // Backspace
    if (sc == 0x2A || sc == 0x36) { shift_down = true; return; }                 // Shift
    if (sc >= 0x3A) return;                                                      // не символ
    char c = (char)sc_ascii[sc];
    if (!c) return;
    if (shift_down) c = apply_shift(c);
    edit_insert(c);
    draw_code();
}

// Открыть редактор (из дока или команды code); при первом открытии — пример
void code_open() {
    win_place(APP_CODE, win_w(680), win_h(500));
    win_raise(APP_CODE);
    code_active = true;
    if (edit_len == 0) {
        // Пример: арифметика + цикл while. Допиши "return a;" и нажми F5/F6.
        const char* demo =
            "int a = 5;\n"
            "int b = 3;\n"
            "a = a * b + 2;\n"
            "int i = 0;\n"
            "while (i < a) {\n"
            "  i = i + 1;\n"
            "}\n";
        int i = 0; while (demo[i]) { edit_buf[i] = demo[i]; i++; }
        edit_len = i;
    }
    edit_cur = edit_len;
    edit_top = 0;
    code_in = 0; code_msg[0] = 0;
    draw_code();
}

// ===========================================================================
void desktop_handle_key(int sc) {
    if (sc == 0xE0) { e0_prefix = true; return; }    // ждём 2-й байт (стрелки)
    bool ext = e0_prefix; e0_prefix = false;
    if (sc & 0x80) return;                           // отпускания не интересуют

    if (ext) {                                       // расширенные клавиши
        if (sc == 0x4B && dock_sel > 0) {            // Left  — влево по доку
            dock_sel--;
            draw_dock_area();                        // только док, а не весь экран
        }
        if (sc == 0x4D && dock_sel < DOCK_N - 1) {   // Right — вправо по доку
            dock_sel++;
            draw_dock_area();
        }
        return;
    }
    if (sc == 0x1C) {                                // Enter — запустить приложение
        dock_launch(dock_sel);                       // (уже открытое — просто поднять)
        return;
    }
    if (sc == SC_F3) cmd_open();                     // F3 — привычный быстрый путь к CMD
}

void about_handle_key(int sc) {
    if (sc == 0xE0) { e0_prefix = true; return; }
    bool ext = e0_prefix; e0_prefix = false;
    if (sc & 0x80) return;                           // отпускания не интересуют
    if (!ext && (sc == 0x1C || sc == 0x01 || sc == SC_F3)) {   // Enter / Esc / F3
        about_active = false;
        win_close_app(APP_ABOUT);
        draw_desktop();                              // вернуть рабочий стол
    }
}

// ===========================================================================
//  КОМПОЗИТОР РАБОЧЕГО СТОЛА (несколько окон) + РАБОТА МЫШЬЮ
// ===========================================================================
// «Источник истины» о том, открыто ли окно — флаги *_active приложений.
bool app_is_open(int a) {
    if (a == APP_CMD)   return cmd_active;
    if (a == APP_FILES) return files_active;
    if (a == APP_SET)   return set_active;
    if (a == APP_TASKS) return tasks_active;
    if (a == APP_CODE)  return code_active;
    if (a == APP_GAME)  return game_active;
    if (a == APP_ABOUT) return about_active;
    if (a == APP_INSTALL) return inst_active;
    return false;
}

// Нарисовать одно окно (каждое рисует себя по своим win_x/win_y)
void draw_app(int a) {
    if      (a == APP_CMD)   { console_set_pos(win_x[a], win_y[a]); console_repaint(); }
    else if (a == APP_FILES) draw_files();
    else if (a == APP_SET)   draw_settings();
    else if (a == APP_TASKS) draw_tasks();
    else if (a == APP_CODE)  draw_code();
    else if (a == APP_GAME)  draw_game();
    else if (a == APP_ABOUT) { ABT_X = win_x[a]; ABT_Y = win_y[a]; draw_about(); }
    else if (a == APP_INSTALL) draw_install();
}

// Рабочий стол целиком: обои + меню-бар + окна по слоям + док (док поверх).
// Это ЕДИНСТВЕННОЕ место, где включается компоновка (comp_full): все окна и
// док рисуются полупрозрачными поверх уже готового кадра.
void draw_desktop() {
    comp_full = true;                        // рисуем полный кадр — стекло можно
    fill_wallpaper();
    draw_menubar();
    for (int z = 1; z <= win_ztop; z++)
        for (int a = 0; a < APP_N; a++)
            if (app_is_open(a) && win_z[a] == z) draw_app(a);
    draw_dock();
    comp_full = false;
}

// Какая иконка дока под точкой (x,y); -1 — ни одна
int dock_hit(int x, int y) {
    int iy = DOCK_Y + DOCK_PAD;
    if (y < iy || y > iy + ICON_SZ) return -1;
    int rx = x - (DOCK_X + DOCK_PAD);
    if (rx < 0) return -1;
    int i = rx / (ICON_SZ + ICON_GAP);
    if (i < 0 || i >= DOCK_N) return -1;
    if (rx % (ICON_SZ + ICON_GAP) > ICON_SZ) return -1;
    return i;
}

// Запустить приложение по номеру иконки дока. Если окно уже открыто —
// просто поднять его (как в настоящей док-панели), а не открывать заново.
void dock_launch(int i) {
    int a = i;                                   // номера иконок = номера приложений
    if (a >= 0 && a < APP_N && app_is_open(a)) { win_raise(a); draw_desktop(); return; }
    if      (i == 0) cmd_open();
    else if (i == 1) files_open();
    else if (i == 2) set_open();
    else if (i == 3) tasks_open();
    else if (i == 4) code_open();
    else if (i == 5) game_open();
    else             about_open();
}

// Самое верхнее окно под точкой; -1 — окна нет
int win_hit(int x, int y) {
    int best = -1, bz = 0;
    for (int a = 0; a < APP_N; a++) {
        if (!app_is_open(a)) continue;
        if (x < win_x[a] || x >= win_x[a] + win_wp[a]) continue;
        if (y < win_y[a] || y >= win_y[a] + win_hp[a]) continue;
        if (win_z[a] > bz) { bz = win_z[a]; best = a; }
    }
    return best;
}

// Закрыть окно приложения мышью (то же, что Esc в этом окне)
void win_close(int a) {
    if      (a == APP_CMD)   cmd_active = false;
    else if (a == APP_FILES) files_active = false;
    else if (a == APP_SET)   set_active = false;
    else if (a == APP_TASKS) tasks_active = false;
    else if (a == APP_CODE)  code_active = false;
    else if (a == APP_GAME)  game_active = false;
    else if (a == APP_ABOUT) about_active = false;
    else if (a == APP_INSTALL) inst_active = false;
    win_close_app(a);
    draw_desktop();
}

// F1 — переключить окно: по кругу (сверху вниз), затем рабочий стол (док)
void focus_cycle() {
    int order[APP_N], n = 0;
    for (int z = win_ztop; z >= 1; z--)
        for (int a = 0; a < APP_N; a++)
            if (app_is_open(a) && win_z[a] == z) order[n++] = a;
    if (n == 0) return;
    int cur = -1;
    for (int i = 0; i < n; i++) if (order[i] == win_focus) cur = i;
    if (cur < 0)          win_raise(order[0]);        // с рабочего стола — на верхнее окно
    else if (cur + 1 < n) win_raise(order[cur + 1]);
    else                  win_focus = -1;              // после последнего — рабочий стол
    draw_desktop();
}

// Мышь: наведение на док и клики (док, шапки окон, «светофор», перетаскивание).
// Курсор на время работы прячет/показывает вызывающий (главный цикл).
void mouse_handle() {
    static int prev_btn = 0, drag = -1, drag_dx = 0, drag_dy = 0;
    int b = mouse_btn;
    int press = b & ~prev_btn;                 // только что нажатые кнопки
    int rel   = prev_btn & ~b;                 // только что отпущенные
    prev_btn = b;
    if (rel & 1) drag = -1;                    // отпустили левую — тащить нечего

    // --- перетаскивание окна за шапку ---
    if (drag >= 0 && (b & 1)) {
        int nx = mouse_x - drag_dx, ny = mouse_y - drag_dy;
        if (nx < 60 - win_wp[drag]) nx = 60 - win_wp[drag];
        if (nx > W - 60) nx = W - 60;
        if (ny < 22) ny = 22;
        if (ny + win_hp[drag] > H - 70) ny = H - 70 - win_hp[drag];
        if (ny < 22) ny = 22;
        if (nx != win_x[drag] || ny != win_y[drag]) {
            win_x[drag] = nx; win_y[drag] = ny;
            draw_desktop();
        }
        return;
    }

    // --- подсветка иконки дока под курсором ---
    // Перерисовываем ТОЛЬКО док: полный кадр на каждое движение мыши заставлял
    // экран мигать и тормозить (окна до дока всё равно не достают).
    int hov = dock_hit(mouse_x, mouse_y);
    if (hov >= 0 && hov != dock_sel) { dock_sel = hov; draw_dock_area(); }

    if (!(press & 1)) return;                  // дальше — только щелчок левой кнопкой

    // Один полный кадр на клик: его нарисует сам запуск (открытие или подъём окна)
    if (hov >= 0) { dock_sel = hov; dock_launch(hov); return; }

    int a = win_hit(mouse_x, mouse_y);
    if (a < 0) return;
    int dx = mouse_x - win_x[a], dy = mouse_y - win_y[a];

    // «светофор» в шапке: красная кнопка (x+11,y+9, r=3) — закрыть окно
    if (dy < TITLE_H && (dx - 11) * (dx - 11) + (dy - 9) * (dy - 9) <= 36) {
        win_close(a);
        return;
    }
    if (win_focus != a) { win_raise(a); draw_desktop(); }   // щелчок — окно наверх
    if (dy < TITLE_H) {                        // и если попали в шапку — тащим
        drag = a;
        drag_dx = dx;
        drag_dy = dy;
        return;
    }
    if (a == APP_INSTALL) inst_click(dx, dy);  // мастер установки: строки и кнопки
}

// ===========================================================================
//  РЕЖИМ ПОЛЬЗОВАТЕЛЯ (ring 3) — КАК В UBUNTU: ЯДРО И ПРОГРАММЫ РАЗДЕЛЕНЫ
// ===========================================================================
//  Принцип Linux: в ring 0 (ядро) лежит только то, что обязано трогать
//  железо; всё остальное — обычные программы в ring 3, и с ядром они
//  говорят ТОЛЬКО через системные вызовы (int 0x80: eax — номер,
//  ebx/ecx/edx — аргументы; ответ приходит в eax).
//
//  Что для этого нужно (сделано ниже):
//    * страницы памяти по 4 МБ (PSE): 0..4 МБ — ядро, 4..8 МБ — программы;
//    * видеопамять отображена и программам (как /dev/fb0 в Linux):
//      рисовать можно напрямую, пиксель стоит дёшево;
//    * GDT: сегменты ring 0 (0x08/0x10) и ring 3 (0x1B/0x23) + TSS —
//      в TSS процессор смотрит, какой стек ядра взять при входе в ядро;
//    * IDT: шлюз int 0x80 (доступен программам) и ловушки ошибок CPU;
//    * сама программа — ОТДЕЛЬНЫЙ бинарник (user_demo.asm): ядро копирует
//      его в 4..8 МБ и уходит туда через iret; выход — сисколл EXIT.
//
//  Наши системные вызовы:
//    0 EXIT  — вернуться в ядро          3 TICKS — миллисекунды с запуска
//    1 KEY   — сканкод клавиши или -1    5 FB    — структура видеопамяти
//    2 MOUSE — x, y, кнопка              6 PRINT — печать в консоль ядра
// ===========================================================================

#define PTE_P    0x1                        // страница присутствует
#define PTE_RW   0x2                        // можно писать
#define PTE_USER 0x4                        // доступна ring 3
#define PTE_PS   0x80                       // большая страница — 4 МБ

#define USER_BASE 0x00400000u               // 4 МБ: сюда грузим программу
#define USER_SIZE 0x00400000u               // 4 МБ: сколько ей отдаём

// Структура «где видеопамять» — её заполняет ядро по вызову SYS_FB
struct sys_fb_t { unsigned int addr; int w, h, bpp, pitch; };

// Каталог страниц: 1024 записи (по 4 МБ каждая). Выровнен на 4096 — так
// требует процессор для регистра CR3.
unsigned int page_dir[1024] __attribute__((aligned(4096)));

// TSS — «шпаргалка» процессора: отсюда он берёт стек ядра (ss0/esp0),
// когда программа из ring 3 входит в ядро (int 0x80). Поля обязаны идти
// строго по порядку, размер структуры — ровно 104 байта.
struct tss_t {
    unsigned int prev, esp0, ss0, esp1, ss1, esp2, ss2;
    unsigned int cr3, eip, eflags, eax, ecx, edx, ebx, esp, ebp, esi, edi;
    unsigned int es, cs, ss, ds, fs, gs, ldt;
    unsigned short trap, iomap;
} __attribute__((packed));

unsigned char gdt_tab[6 * 8] __attribute__((aligned(16)));      // 6 сегментов
unsigned char idt_tab[256 * 8] __attribute__((aligned(16)));    // 256 шлюзов
tss_t tss;
unsigned int sys_stack[512] __attribute__((aligned(16)));       // стек ядра для сисколлов

// ===========================================================================
//  Служебный ассемблер: вход в ring 3, возврат в ядро, шлюз int 0x80
// ===========================================================================
//  Как в Linux, здесь самый минимум: один шлюз сисколлов (сохраняет все
//  регистры программы) и ловушки #DF/#GP/#PF — они печатают вектор и код
//  ошибки, а потом останавливают машину (вместо загадочной перезагрузки).
extern "C" void enter_user(unsigned int entry, unsigned int ustack);
extern "C" void syscall_back();
extern "C" void syscall_dispatch(unsigned int* regs);
extern "C" void cpu_fault(unsigned int vec, unsigned int err);
extern "C" void isr_syscall();
extern "C" void isr_df(); extern "C" void isr_gp(); extern "C" void isr_pf();

unsigned int saved_kernel_esp = 0;          // куда вернуться после ring 3

asm(
".text\n"
// --- войти в ring 3: [esp+4]=вход, [esp+8]=стек программы ---------------
".global _enter_user\n"
"_enter_user:\n"
"    pushl %ebp\n"
"    pushl %ebx\n"
"    pushl %esi\n"
"    pushl %edi\n"
"    movl %esp, _saved_kernel_esp\n"
"    movl 20(%esp), %eax\n"                 // EIP программы
"    movl 24(%esp), %edx\n"                 // ESP программы
"    movw $0x23, %cx\n"                     // данные программы (ring 3)
"    movw %cx, %ds\n"
"    movw %cx, %es\n"
"    movw %cx, %fs\n"
"    movw %cx, %gs\n"
"    pushl $0x23\n"                         // SS программы
"    pushl %edx\n"                          // ESP программы
"    pushl $0x2\n"                          // EFLAGS (прерывания выключены)
"    pushl $0x1B\n"                         // CS программы (ring 3)
"    pushl %eax\n"                          // EIP
"    iret\n"
// --- вернуться в ядро (зовётся из сисколла EXIT) ------------------------
".global _syscall_back\n"
"_syscall_back:\n"
"    movl _saved_kernel_esp, %esp\n"
"    popl %edi\n"
"    popl %esi\n"
"    popl %ebx\n"
"    popl %ebp\n"
"    ret\n"
// --- шлюз int 0x80: сохранить ВСЁ, взять сегменты ядра, позвать C --------
".global _isr_syscall\n"
"_isr_syscall:\n"
"    pushl %ds\n"
"    pushl %es\n"
"    pushl %fs\n"
"    pushl %gs\n"
"    pushal\n"
"    movl $0x10, %eax\n"
"    movl %eax, %ds\n"
"    movl %eax, %es\n"
"    movl %eax, %fs\n"
"    movl %eax, %gs\n"
"    pushl %esp\n"                          // аргумент: кадр с регистрами
"    call _syscall_dispatch\n"
"    addl $4, %esp\n"
"    popal\n"                               // EAX тут — уже результат вызова
"    popl %gs\n"
"    popl %fs\n"
"    popl %es\n"
"    popl %ds\n"
"    iret\n"
// --- ошибки процессора: вектор + код ошибки -> C, дальше стоп -----------
".global _isr_df\n"
"_isr_df:\n"
"    pushl $8\n"
"    jmp _isr_common\n"
".global _isr_gp\n"
"_isr_gp:\n"
"    pushl $13\n"
"    jmp _isr_common\n"
".global _isr_pf\n"
"_isr_pf:\n"
"    pushl $14\n"
"    jmp _isr_common\n"
"_isr_common:\n"
"    pushal\n"
"    movl $0x10, %eax\n"
"    movl %eax, %ds\n"
"    movl %eax, %es\n"
"    movl %eax, %fs\n"
"    movl %eax, %gs\n"
"    movl 32(%esp), %ecx\n"                 // вектор (положили его сами)
"    movl 36(%esp), %ebx\n"                 // код ошибки от процессора
"    pushl %ebx\n"
"    pushl %ecx\n"
"    call _cpu_fault\n"
"    addl $8, %esp\n"
"1:\n"
"    hlt\n"
"    jmp 1b\n"
);

// ===========================================================================
//  Инициализация: страницы, GDT/TSS, IDT
// ===========================================================================
// Включить страничную адресацию. Без неё ring 3 невозможен — точно так же,
// как в Linux: у каждой программы своё адресное пространство.
void mem_init(unsigned int fba) {
    for (int i = 0; i < 1024; i++) page_dir[i] = 0;
    page_dir[0] = 0x00000000 | PTE_P | PTE_RW | PTE_PS;             // 0..4 МБ — ядро (только ring 0)
    page_dir[1] = USER_BASE  | PTE_P | PTE_RW | PTE_PS | PTE_USER;  // 4..8 МБ — программы (ring 3)
    unsigned int base = fba & 0xFFC00000u;   // адрес видеопамяти — до 4 МБ
    for (int i = 0; i < 3; i++)              // картину видеопамяти открываем и программам
        page_dir[(base >> 22) + i] = (base + (unsigned int)i * 0x100000) | PTE_P | PTE_RW | PTE_PS | PTE_USER;
    asm volatile("mov %%cr4, %%eax\n\tor $0x10, %%eax\n\tmov %%eax, %%cr4" ::: "eax");       // включить страницы 4 МБ (PSE)
    asm volatile("mov %0, %%cr3" :: "r"(page_dir) : "memory");                                // каталог страниц
    asm volatile("mov %%cr0, %%eax\n\tor $0x80000000, %%eax\n\tmov %%eax, %%cr0" ::: "eax"); // и сами страницы (PG)
}

// Записать сегмент в GDT: base — начало, limit — размер, access — права,
// flags — разрядность и гранулярность (0xCF = 4 ГБ страницами по 4 КБ)
void gdt_set(int i, unsigned int base, unsigned int limit, unsigned char access, unsigned char flags) {
    unsigned char* d = gdt_tab + i * 8;
    d[0] = (unsigned char)(limit & 0xFF);
    d[1] = (unsigned char)((limit >> 8) & 0xFF);
    d[2] = (unsigned char)(base & 0xFF);
    d[3] = (unsigned char)((base >> 8) & 0xFF);
    d[4] = (unsigned char)((base >> 16) & 0xFF);
    d[5] = access;
    d[6] = (unsigned char)(((limit >> 16) & 0x0F) | flags);
    d[7] = (unsigned char)((base >> 24) & 0xFF);
}

// GDT + TSS: сегменты ядра (0x08/0x10), программы (0x1B/0x23) и TSS (0x28)
void gdt_init() {
    for (int i = 0; i < 48; i++) gdt_tab[i] = 0;
    gdt_set(1, 0, 0xFFFFF, 0x9A, 0xCF);      // 0x08 — код ядра
    gdt_set(2, 0, 0xFFFFF, 0x92, 0xCF);      // 0x10 — данные ядра
    gdt_set(3, 0, 0xFFFFF, 0xFA, 0xCF);      // 0x1B — код программы (ring 3)
    gdt_set(4, 0, 0xFFFFF, 0xF2, 0xCF);      // 0x23 — данные программы
    for (unsigned int i = 0; i < sizeof(tss); i++) ((unsigned char*)&tss)[i] = 0;
    tss.ss0  = 0x10;                          // стек ядра — сегмент данных ядра
    tss.esp0 = (unsigned int)(sys_stack + 512);
    tss.iomap = (unsigned short)sizeof(tss);  // I/O-карты нет: порты программам закрыты
    gdt_set(5, (unsigned int)&tss, sizeof(tss) - 1, 0x89, 0x00);
    struct { unsigned short lim; unsigned int base; } __attribute__((packed)) gdtr;
    gdtr.lim = sizeof(gdt_tab) - 1;
    gdtr.base = (unsigned int)gdt_tab;
    asm volatile("lgdt %0" :: "m"(gdtr));
    asm volatile("ljmp $0x08, $1f\n1:");      // перезагрузить CS
    asm volatile("mov $0x10, %%ax\n\tmov %%ax, %%ds\n\tmov %%ax, %%es\n\tmov %%ax, %%fs\n\tmov %%ax, %%gs\n\tmov %%ax, %%ss" ::: "eax", "memory");
    asm volatile("mov $0x28, %%ax\n\tltr %%ax" ::: "eax");   // пусть процессор знает про TSS
}

// Шлюз в таблице прерываний. dpl=3 — шлюз доступен программам (int 0x80).
void idt_set(int n, unsigned int handler, unsigned char dpl) {
    unsigned char* d = idt_tab + n * 8;
    for (int i = 0; i < 8; i++) d[i] = 0;
    if (!handler) return;
    d[0] = (unsigned char)(handler & 0xFF);
    d[1] = (unsigned char)((handler >> 8) & 0xFF);
    d[2] = 0x08;                               // селектор кода ядра
    d[5] = (unsigned char)(0x8E | (dpl << 5)); // шлюз прерывания + уровень доступа
    d[6] = (unsigned char)((handler >> 16) & 0xFF);
    d[7] = (unsigned char)((handler >> 24) & 0xFF);
}

void idt_init() {
    idt_set(0x80, (unsigned int)isr_syscall, 3);   // сисколлы — можно из ring 3
    idt_set(8,  (unsigned int)isr_df, 0);          // double fault
    idt_set(13, (unsigned int)isr_gp, 0);          // общая ошибка защиты
    idt_set(14, (unsigned int)isr_pf, 0);          // страничная ошибка
    struct { unsigned short lim; unsigned int base; } __attribute__((packed)) idtr;
    idtr.lim = sizeof(idt_tab) - 1;
    idtr.base = (unsigned int)idt_tab;
    asm volatile("lidt %0" :: "m"(idtr));
}

// ===========================================================================
//  Сами системные вызовы: единственная дверь из программы в ядро
// ===========================================================================
// Проверка: адрес и длина обязаны лежать в памяти программы (4..8 МБ).
// Программа не может попросить ядро «почитать» чужую память.
bool user_ok(unsigned int addr, unsigned int len) {
    if (addr < USER_BASE) return false;
    if (len > USER_SIZE) return false;
    if (addr + len > USER_BASE + USER_SIZE) return false;
    return true;
}

unsigned int sys_do(unsigned int num, unsigned int a1, unsigned int a2, unsigned int a3) {
    (void)a2; (void)a3;
    if (num == 0) { syscall_back(); return 0; }               // EXIT — назад в ядро (не вернётся)
    if (num == 1) return (unsigned int)poll_key();            // KEY — сканкод или -1
    if (num == 2) {                                           // MOUSE — {x, y, кнопка}
        if (!user_ok(a1, 12)) return (unsigned int)-1;
        unsigned int* p = (unsigned int*)a1;
        p[0] = (unsigned int)mouse_x;
        p[1] = (unsigned int)mouse_y;
        p[2] = (unsigned int)(mouse_btn & 1);
        return 0;
    }
    if (num == 3) return millis();                            // TICKS
    if (num == 4) return (unsigned int)day_secs();            // TIME
    if (num == 5) {                                           // FB — где видеопамять
        if (!user_ok(a1, sizeof(sys_fb_t))) return (unsigned int)-1;
        sys_fb_t* p = (sys_fb_t*)a1;
        p->addr  = (unsigned int)fb;
        p->w = W; p->h = H; p->bpp = FB_BPP; p->pitch = FB_PITCH;
        return 0;
    }
    if (num == 6) {                                           // PRINT — строка в консоль ядра
        if (!user_ok(a1, 1)) return (unsigned int)-1;
        const char* s = (const char*)a1;
        char buf[256];
        int n = 0;
        while (n < 255 && user_ok(a1 + n, 1) && s[n]) { buf[n] = s[n]; n++; }
        buf[n] = 0;
        cmd_print(buf);
        return (unsigned int)n;
    }
    return (unsigned int)-1;                                  // неизвестный вызов
}

// Мостик из ассемблерного шлюза в C: кадр регистров (pushal) лежит по
// адресу regs; ответ кладём в сохранённый EAX — его вернёт popal.
extern "C" void syscall_dispatch(unsigned int* regs) {
    unsigned int num = regs[7];                               // EAX — номер вызова
    unsigned int a1  = regs[4];                               // EBX
    unsigned int a2  = regs[6];                               // ECX
    unsigned int a3  = regs[5];                               // EDX
    regs[7] = sys_do(num, a1, a2, a3);
}

// Аварийный выход: напечатать, что случилось, и остановить машину
extern "C" void cpu_fault(unsigned int vec, unsigned int err) {
    unsigned int cr2 = 0;
    asm volatile("mov %%cr2, %0" : "=r"(cr2));
    cmd_print("CPU fault: vector ");
    print_dec(vec);
    cmd_print(", error code ");
    print_dec(err);
    cmd_print(" at ");
    print_dec(cr2);
    cmd_print(" (pde ");
    print_dec(page_dir[cr2 >> 22]);
    cmd_print(")\nSystem halted.\n");
    while (true) { }
}

// ===========================================================================
//  Запуск программы пользователя: скопировать в 4..8 МБ и прыгнуть в ring 3
// ===========================================================================
bool user_start(const unsigned char* img, unsigned int len) {
    if (len == 0 || len > USER_SIZE - 0x10000) return false;
    unsigned char* dst = (unsigned char*)USER_BASE;
    for (unsigned int i = 0; i < len; i++) dst[i] = img[i];
    unsigned int ustack = USER_BASE + USER_SIZE - 8;          // вершина стека программы
    unsigned int vcr0, vcr3, vcr4;
    asm volatile("mov %%cr0, %0" : "=r"(vcr0));
    asm volatile("mov %%cr3, %0" : "=r"(vcr3));
    asm volatile("mov %%cr4, %0" : "=r"(vcr4));
    cmd_print("ring3: cr0="); print_dec(vcr0);
    cmd_print(" cr3="); print_dec(vcr3);
    cmd_print(" cr4="); print_dec(vcr4);
    cmd_print(" pd0="); print_dec(page_dir[0]);
    cmd_print(" pd1="); print_dec(page_dir[1]);
    cmd_print("\n");
    cmd_print("ring3: entering user mode...\n");
    enter_user(USER_BASE, ustack);                            // уходим в ring 3
    cmd_print("ring3: back in the kernel (ring 0).\n");
    return true;
}

// ===========================================================================
//  kernel_main — точка входа ядра (её вызывает stage2.asm)
// ===========================================================================
extern "C" void kernel_main() {
    init_video();                        // видео: «записка» stage2 (VESA VBE)
    mem_init((unsigned int)fb);          // страницы: ядро 0..4 МБ, программы 4..8 МБ
    gdt_init();                          // сегменты ring 0/3 + TSS (как в Linux)
    idt_init();                          // шлюз сисколлов int 0x80 + ловушки ошибок
    pit_init();                          // таймер (заставка, игра, мигание курсора)
    draw_splash();                       // заставка с логотипом
    init_layout();                       // окно консоли — под текущий экран
    init_desktop_layout();               // док и About — по центру экрана
    mouse_init();                        // мышь PS/2 (порт 0x64/0x60)
    mouse_x = W / 2; mouse_y = H / 2;    // курсор — в центр экрана
    cmd_active = false;
    about_active = false;
    files_active = false;
    set_active = false;
    tasks_active = false;
    game_active = false;
    code_active = false;
    for (int a = 0; a < APP_N; a++) win_z[a] = 0;
    win_focus = -1; win_ztop = 0;
    draw_desktop();                      // рабочий стол: обои, меню-бар, док
    cursor_show();                       // и стрелка-курсор поверх всего
    boot_secs = day_secs();              // запомнили время запуска — для «Uptime»

    // Главный цикл: мышь + клавиатура; между нажатиями обновляем живые окна
    while (true) {
        if (mouse_ok && mouse_read()) {              // пришли данные от мыши
            cursor_hide();
            mouse_handle();
            cursor_show();
            continue;
        }
        int sc = poll_key();
        if (sc < 0) {                                // нет клавиш — «тикаем»
            millis();                                // таймер (заставка, игра, курсор)
            clock_tick();                            // часы в меню-баре
            if (win_focus == APP_TASKS && tasks_active) tasks_tick();   // «Uptime»
            if (win_focus == APP_GAME  && game_active)  game_tick();    // шаг змейки
            continue;                                // курсор при этом не трогаем
        }

        cursor_hide();
        if (sc == 0x3B) {                            // F1 — переключить окно
            focus_cycle();
            cursor_show();
            continue;
        }
        int f = win_focus;                           // клавиатура — у окна в фокусе
        if      (f == APP_GAME  && game_active)  game_handle_key(sc);
        else if (f == APP_CODE  && code_active)  code_handle_key(sc);
        else if (f == APP_TASKS && tasks_active) tasks_handle_key(sc);
        else if (f == APP_FILES && files_active) files_handle_key(sc);
        else if (f == APP_SET   && set_active)   set_handle_key(sc);
        else if (f == APP_ABOUT && about_active) about_handle_key(sc);
        else if (f == APP_INSTALL && inst_active) inst_handle_key(sc);  // мастер установки
        else if (f == APP_CMD   && cmd_active)   cmd_handle_key(sc);
        else                                     desktop_handle_key(sc);
        cursor_show();
    }
}
