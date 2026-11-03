// ===========================================================================
//  SHELL s1d: ВИДЕО, ОБОИ, РАСКЛАДКА, РАМКА ОКНА
// ===========================================================================
//  Сюда переехала вторая половина прежнего s1_glass.cpp: всё, что зависит от
//  РАЗМЕРА ЭКРАНА. Разделение такое:
//
//    s1_ui       — инструментарий рисования (скругления, стекло, иконки, тема);
//    s1b_wall    — обои Aqua (панорама гор);
//    s1c_text    — очередь клавиатуры и строковые хелперы;
//    s1d_layout  — ЭТОТ ФАЙЛ: где кадровый буфер, обои Night Ridge/Bliss и
//                  геометрия окна консоли, доки/шапки, рамка окна.
//
//  Порядок включения: после s1_ui (нужны fill_glass/dither_over/draw_round_rect)
//  и после s1b_wall (fill_wallpaper_aqua).
// ===========================================================================

// ===========================================================================
//  ОБОИ: быстрая заливка всего экрана
// ===========================================================================
//  Чистая wallpaper_pixel() в каждом пикселе гоняет целочисленный рельеф
//  «с нуля» — на 1024x768 это миллионы делений, и кадр занимает секунды.
//  Поэтому рисуем по частям:
//    1) рельеф трёх хребтов считаем ОДИН РАЗ в таблицы по колонкам;
//    2) тела хребтов заливаем колонками через fill_rect — это чистый rep stosl;
//    3) небо со звёздами и луной — только для видимых пикселей.
//  Результат тот же, что у wallpaper_pixel, но в разы быстрее.
static int rw_top[3][RW_MAXW];           // вершины трёх хребтов по колонкам
static int rw_usew = 0, rw_useh = 0;     // под какой экран таблицы
static int rw_theme = -1;                // под какую тему считали таблицы

static void wallpaper_precompute() {
    rw_usew = W; rw_useh = H;
    if (rw_usew > RW_MAXW) rw_usew = RW_MAXW;
    for (int x = 0; x < rw_usew; x++) {
        rw_top[0][x] = ridge_at(x, 0);
        rw_top[1][x] = ridge_at(x, 1);
        rw_top[2][x] = ridge_at(x, 2);
    }
}

// Дневная тема «Bliss Hill» — композиция из дешёвых примитивов:
// небо целыми строками (цвет зависит только от y), холмы FAR/MID колонками
// с боковой подсветкой, трава ближнего слоя строками с полосами по хэшу.
static void fill_wallpaper_day() {
    for (int y = 0; y < H; y++)
        fill_rect(0, y, W, 1, sky_color(y));
    static const unsigned int db[2] = { BW_FAR, BW_MID };
    static const unsigned int dl[2] = { BW_FAR_L, BW_MID_L };
    static const unsigned int de[2] = { BW_FAR_E, BW_MID_E };
    for (int layer = 0; layer < 2; layer++) {
        for (int x = 0; x < rw_usew; x++) {
            int t = rw_top[layer][x];
            if (t < 0) t = 0;
            if (t < H) {
                fill_rect(x, t, 1, H - t, mix_col(db[layer], dl[layer], x * 96 / (W + 1)));
                put_pixel(x, t, de[layer]);
                if (t + 1 < H) put_pixel(x, t + 1, de[layer]);
            }
        }
    }
    int ymin = H;
    for (int x = 0; x < rw_usew; x++) { int t = rw_top[2][x]; if (t < ymin) ymin = t; }
    if (ymin < 0) ymin = 0;
    for (int y = ymin; y < H; y++)
        for (int x = 0; x < W; x++)
            if (y >= rw_top[2][x < rw_usew ? x : rw_usew - 1])
                put_pixel(x, y, grass_pixel(x, y, rw_top[2][x < rw_usew ? x : rw_usew - 1]));
}

// Ночная тема «Night Ridge»: небо по колонкам (звёзды, луна), хребты — заливкой.
static void fill_wallpaper_night() {
    int n = FB_BPP / 8;
    for (int x = 0; x < W; x++) {
        int skyend = rw_top[0][x < rw_usew ? x : rw_usew - 1];
        if (skyend > H) skyend = H;
        if (skyend <= 0) continue;
        for (int y = 0; y < skyend; y++) {
            unsigned int c = sky_color(y);
            if (y < (H * 48) / 100) {
                unsigned int st = star_color(x, y);
                if (st) c = st;
            }
            unsigned int m = moon_color(x, y, c);
            if (m) c = m;
            unsigned char* p = fb + y * FB_PITCH + x * n;
            unsigned int v = encode_color(c);
            if (n == 4) *(volatile unsigned int*)p = v;
            else { p[0] = (unsigned char)(v & 0xFF); p[1] = (unsigned char)((v >> 8) & 0xFF); p[2] = (unsigned char)((v >> 16) & 0xFF); }
        }
    }
    static const unsigned int bod[3] = { RW_FAR, RW_MID, RW_NEAR };
    static const unsigned int edg[3] = { RW_FAR_E, RW_MID_E, RW_NEAR_E };
    for (int layer = 0; layer < 3; layer++) {
        for (int x = 0; x < W; x++) {
            int t = rw_top[layer][x < rw_usew ? x : rw_usew - 1];
            if (t < 0) t = 0;
            if (t < H) {
                fill_rect(x, t, 1, H - t, bod[layer]);
                put_pixel(x, t, edg[layer]);
                if (t + 1 < H) put_pixel(x, t + 1, edg[layer]);
            }
        }
    }
}

// Название активного набора обоев (окно Settings показывает его строкой).
const char* wall_theme_name() {
    if (wall_theme == 1) return "Bliss Hill (grass)";
    if (wall_theme == 2) return "Aqua (mountain dusk)";
    return "Night Ridge (moon)";
}

// Цвет обоев в точке для АКТИВНОГО набора: им «стирают» временную графику
// (курсор, подпись дока), поэтому функция обязана совпадать с fill_wallpaper.
unsigned int wallpaper_scene_pixel(int x, int y) {
    if (wall_theme == 2) return aqua_pixel(x, y);
    return wallpaper_pixel(x, y);
}

// Залить весь экран обоями активного набора (рабочий стол).
void fill_wallpaper() {
    if (wall_theme == 2) { fill_wallpaper_aqua(); return; }   // панорама Aqua
    if (rw_usew != W || rw_useh != H || rw_theme != wall_theme) {
        wallpaper_precompute();                     // рельеф зависит и от темы
        rw_theme = wall_theme;
    }
    if (wall_theme == 1) fill_wallpaper_day();
    else                 fill_wallpaper_night();
}

// ===========================================================================
//  ВИДЕО: где кадровый буфер — спрашиваем у ЯДРА (SYS_FB, как /dev/fb0)
// ===========================================================================
//  Ядро приняло «записку» stage2 при старте и отдаёт готовую структуру
//  myos_fb_t. Оболочка рисует пиксели напрямую — на кадры не нужны ни вызовы,
//  ни копии.
void init_video() {
    myos_fb_t f;
    sys_get_fb(&f);                          // int 0x80, SYS_FB
    if (!f.addr || !f.pitch || !f.w || !f.h) return;   // видеопамяти нет
    fb = (unsigned char*)f.addr;             // рисуем прямо в LFB
    FB_PITCH = f.pitch; FB_BPP = f.bpp; W = f.w; H = f.h;
    FB_RPOS = f.rpos; FB_GPOS = f.gpos; FB_BPOS = f.bpos;  // порядок каналов VBE
}

// --- Геометрия окна консоли (плавающее поверх обоев) ---
int WIN_X, WIN_Y, WIN_W, WIN_H;             // окно консоли
const int WIN_R = 8;                        // радиус скругления углов
const int TITLE_H = 26;                     // высота шапки окна («светофор»)
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
    if (WIN_W < 64) WIN_W = 64;             // защита от крошечного экрана
    if (WIN_H < 64) WIN_H = 64;
    TXT_X = WIN_X + 6;                      // левый верх текстовой области
    TXT_Y = WIN_Y + TITLE_H + 6;
    TXT_W = WIN_W - 12;
    TXT_H = WIN_H - TITLE_H - 12;
    COLS = TXT_W / 8;                       // колонок (знакоместо 8 пикселей)
    ROWS = TXT_H / 16;                      // строк (высота строки 16)
    if (COLS > CG_COLS) COLS = CG_COLS;     // но не больше теневой копии текста
    if (ROWS > CG_ROWS) ROWS = CG_ROWS;
    if (COLS < 1) COLS = 1;
    if (ROWS < 1) ROWS = 1;
}

// Плотность «стекла» (из 256): чем больше, тем менее прозрачно.
const int GLASS       = 226;    // тело окна
const int GLASS_TITLE = 165;    // шапка окна
const int GLASS_DOCK  = 165;    // плашка дока
const int GLASS_ICON  = 130;    // подложка иконки
const int GLASS_ICON_HV = 200;  // подложка под курсором
const int GLASS_MBAR  = 175;    // меню-бар
const int GLASS_TILE  = 185;    // плитка Activities

// В фокусе ли окно, рисуемое ПРЯМО СЕЙЧАС. Выставляет draw_app (s11) перед
// отрисовкой каждого приложения, чтобы рамка в фокусе была ярче.
bool g_frame_focus = true;

// Нарисовать плавающее окно: стеклянное тело, шапка, рамка, «светофор».
//    - тело: почти чёрное стекло (сквозь него чуть видно фон);
//    - шапка: светлее и полупрозрачнее тела («матовое стекло»);
//    - «светофор»: красная — закрыть (клик обрабатывает s11), 3 кнопки;
//    - заголовок: по ЦЕНТРУ шапки (как в macOS);
//    - окно не в фокусе — рамка и светофор тусклее.
void draw_window_frame(int x, int y, int w, int h, const char* title) {
    if (w < 16 || h < TITLE_H + 2) return;  // слишком маленькое — не рисуем
    fill_glass(x, y, w, h, WIN_R, C_GLASS_BODY, GLASS);
    fill_glass(x + 1, y + 1, w - 2, TITLE_H - 1, WIN_R - 1, C_GLASS_TITLE, GLASS_TITLE);
    unsigned int edge = g_frame_focus ? C_EDGE_F : C_EDGE;
    draw_round_rect(x, y, w, h, WIN_R, edge);
    dither_over(x + WIN_R, y + 1, w - 2 * WIN_R, 1, C_WHITE, 4);
    int cy = y + TITLE_H / 2;
    unsigned int rc = g_frame_focus ? C_RED    : 0x6B5B58;
    unsigned int yc = g_frame_focus ? C_YELLOW : 0x6F6553;
    unsigned int gc = g_frame_focus ? C_GREEN  : 0x57695A;
    fill_circle(x + 14, cy, 5, rc);
    fill_circle(x + 31, cy, 5, yc);
    fill_circle(x + 48, cy, 5, gc);
    draw_circle(x + 14, cy, 5, C_BLACK);
    draw_circle(x + 31, cy, 5, C_BLACK);
    draw_circle(x + 48, cy, 5, C_BLACK);
    int n = 0; while (title[n]) n++;
    int tx = x + (w - n * 8) / 2;
    if (tx < x + 58) tx = x + 58;                    // не залезать на светофор
    draw_text(tx, y + TITLE_H / 2 - 7, title, TEXT_COL);
    if (comp_full) {                                 // тень — только в полном кадре
        dither_over(x + 2, y + h, w - 4, 1, C_BLACK, 10);
        dither_over(x + 4, y + h + 1, w - 8, 1, C_BLACK, 6);
        dither_over(x + 8, y + h + 2, w - 16, 1, C_BLACK, 3);
    }
}

// Окно консоли — просто частный случай общей функции
void draw_console_window() {
    draw_window_frame(WIN_X, WIN_Y, WIN_W, WIN_H, "MyOS CMD");
}
