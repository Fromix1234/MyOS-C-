// ===========================================================================
//  SHELL s1: glass drawing, video params, layout, window frame
// ===========================================================================
//  MyOS v2.0 module. Split mechanically from kernel.cpp v1.2
//  (git a81d4f4) and then reworked: see README section 11.

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
                // ВАЖНО: a — вес НОВОГО цвета (как в общем 24-бит пути ниже):
                // при a=256 (точечная перерисовка) стекло ложится плотно и
                // СТИРАЕТ старое; при a=alpha (полный кадр) — полупрозрачно поверх фона.
                row[px] = ((((cs_rb * a + (old & 0x00FF00FF) * ia) >> 8) & 0x00FF00FF))
                        | ((((cs_g  * a + (old & 0x0000FF00) * ia) >> 8) & 0x0000FF00));
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

// Залить весь экран свежими обоями (рабочий стол). Обои пер-пиксельные
// (небо, звёзды, луна, три слоя хребтов) — пишем прямо в видеопамять строкой:
// сначала получаем RGB чистой функцией wallpaper_pixel, затем кодируем
// под формат экрана (VESA мог выдать 24 или 32 бита и любой порядок каналов).
void fill_wallpaper() {
    int n = FB_BPP / 8;
    sys_debug("W0");                          // ОТЛАДКА: фазы заливки обоев
    for (int y = 0; y < H; y++) {
        if (y == 128) sys_debug("W1");
        if (y == 256) sys_debug("W2");
        if (y == 384) sys_debug("W3");
        if (y == 512) sys_debug("W4");
        if (y == 640) sys_debug("W5");
        unsigned char* row = fb + y * FB_PITCH;
        for (int x = 0; x < W; x++) {
            unsigned int v = encode_color(wallpaper_pixel(x, y));
            if (n == 4) {
                *(volatile unsigned int*)(row + x * 4) = v;
            } else {
                unsigned char* p = row + x * 3;
                p[0] = (unsigned char)(v & 0xFF);
                p[1] = (unsigned char)((v >> 8) & 0xFF);
                p[2] = (unsigned char)((v >> 16) & 0xFF);
            }
        }
    }
    sys_debug("W6");
}

// ===========================================================================
//  ВИДЕО: где кадровый буфер — спрашиваем у ЯДРА (SYS_FB, как /dev/fb0)
// ===========================================================================
//  В v1.2 «записку» stage2 (0x0504..0x0514) читало ядро; в v2.0 ядро бережно
//  приняло её при старте и отдаёт оболочке готовую структуру myos_fb_t.
//  Оболочка рисует пиксели напрямую — на кадры не нужны ни вызовы, ни копии.
void init_video() {
    myos_fb_t f;
    sys_get_fb(&f);                          // int 0x80, SYS_FB
    if (!f.addr || !f.pitch || !f.w || !f.h) return;   // видеопамяти нет
    fb = (unsigned char*)f.addr;             // рисуем прямо в LFB
    FB_PITCH = f.pitch; FB_BPP = f.bpp; W = f.w; H = f.h;
    FB_RPOS = f.rpos; FB_GPOS = f.gpos; FB_BPOS = f.bpos;  // порядок каналов VBE
}

// --- Геометрия окна консоли (плавающее поверх обоев) ---
// Значения считает init_layout() — окно растягивается под ЛЮБОЙ экран,
// который дал VESA (640x480, 1024x768, 1920x1080...): консоль занимает
// почти весь экран, поля по 18 пикселей слева/справа и 4 снизу.
int WIN_X, WIN_Y, WIN_W, WIN_H;             // окно консоли
const int WIN_R = 8;                        // радиус скругления углов
const int TITLE_H = 26;                     // высота шапки окна (классический «светофор»)
const int MB_H = 24;                        // высота меню-бара (3 строки шрифта)
int TXT_X, TXT_Y, TXT_W, TXT_H;             // текстовая область внутри окна
int COLS, ROWS;                             // её размер в колонках и строках

// Теневая копия текста консоли (чтобы окно можно было перерисовать, когда его
// перекрыли другие окна). Размер — с запасом под 1024x768 и чуть больше.
const int CG_ROWS = 64, CG_COLS = 200;

// Посчитать геометрию под ТЕКУЩИЙ экран (вызывается после init_video).
// Меню-бар занимает верхние MB_H пикселей — окно начинается под ним.
void init_layout() {
    WIN_X = 18;
    WIN_Y = MB_H + 6;                       // меню-бар + отступ
    WIN_W = W - 2 * WIN_X;
    int bottom = (H >= 400) ? 104 : 4;      // место под док снизу
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
// Высота меню-бара (живёт здесь, её же использует init_layout)
// Плотность «стекла» (из 256): чем больше, тем менее прозрачно.
// Тело окна почти непрозрачное — так текст читается идеально чётко,
// а шапка, док, плитки и иконки «стеклянные» заметнее (сквозь них видно фон).
const int GLASS       = 226;    // тело окна
const int GLASS_TITLE = 165;    // шапка окна
const int GLASS_DOCK  = 165;    // плашка дока
const int GLASS_ICON  = 130;    // подложка иконки
const int GLASS_ICON_HV = 200;  // подложка под курсором
const int GLASS_MBAR  = 175;    // меню-бар
const int GLASS_TILE  = 185;    // плитка Activities
const unsigned int TEXT_COL = C_WHITE;      // белый текст

// В фокусе ли окно, рисуемое ПРЯМО СЕЙЧАС. Выставляет draw_app (s11) перед
// отрисовкой каждого приложения, чтобы рамка в фокусе была ярче.
bool g_frame_focus = true;

// Рисуемое сейчас окно в фокусе? (выставляет draw_app; также отвечает клику)
// Пока никто не переопределил — считаем окно в фокусе.

// Нарисовать плавающее окно в точке (x, y): стеклянное тело, шапка, рамка.
// Общая функция для всех окон (консоль, About, приложения) — геометрию
// задаёт вызывающий, а размеры окна он же кладёт в win_wp/win_hp.
//    - тело: почти чёрное стекло (сквозь него чуть видно фон);
//    - шапка: светлее и полупрозрачнее тела («матовое стекло»);
//    - «светофор»: красная — закрыть (клик обрабатывает s11), 3 кнопки;
//    - заголовок: по ЦЕНТРУ шапки (как в macOS);
//    - окно не в фокусе — рамка и светофор тусклее (сразу видно, кто активен).
void draw_window_frame(int x, int y, int w, int h, const char* title) {
    // 1. тело окна — почти чёрное стекло
    fill_glass(x, y, w, h, WIN_R, C_GLASS_BODY, GLASS);
    sys_debug("FB");                         // ОТЛАДКА: фазы рамки окна
    // 2. шапка — светлее и прозрачнее тела
    fill_glass(x + 1, y + 1, w - 2, TITLE_H - 1, WIN_R - 1, C_GLASS_TITLE, GLASS_TITLE);
    sys_debug("FH");
    // 3. контур: в фокусе светлый, вне фокуса тусклый
    unsigned int edge = g_frame_focus ? C_EDGE_F : C_EDGE;
    draw_round_rect(x, y, w, h, WIN_R, edge);
    // 4. блик по верхнему краю шапки (тонкая светлая линия)
    dither_over(x + WIN_R, y + 1, w - 2 * WIN_R, 1, C_WHITE, 4);
    // 5. «светофор» — три кнопки окна слева
    int cy = y + TITLE_H / 2;
    unsigned int rc = g_frame_focus ? C_RED    : 0x6B5B58;
    unsigned int yc = g_frame_focus ? C_YELLOW : 0x6F6553;
    unsigned int gc = g_frame_focus ? C_GREEN  : 0x57695A;
    fill_circle(x + 14, cy, 5, rc);
    fill_circle(x + 31, cy, 5, yc);
    fill_circle(x + 48, cy, 5, gc);
    // тонкое тёмное кольцо вокруг кнопок — для глубины
    draw_circle(x + 14, cy, 5, C_BLACK);
    draw_circle(x + 31, cy, 5, C_BLACK);
    draw_circle(x + 48, cy, 5, C_BLACK);
    // 6. заголовок окна — по центру шапки
    int n = 0; while (title[n]) n++;
    int tx = x + (w - n * 8) / 2;
    draw_text(tx, y + TITLE_H / 2 - 7, title, TEXT_COL);
    // 7. мягкая тень под окном (только в полном кадре: вне его область «чужая»)
    if (comp_full) {
        dither_over(x + 2, y + h, w - 4, 1, C_BLACK, 10);
        dither_over(x + 4, y + h + 1, w - 8, 1, C_BLACK, 6);
        dither_over(x + 8, y + h + 2, w - 16, 1, C_BLACK, 3);
    }
    sys_debug("FS");
}

// Окно консоли — просто частный случай общей функции
void draw_console_window() {
    draw_window_frame(WIN_X, WIN_Y, WIN_W, WIN_H, "MyOS CMD");
}
