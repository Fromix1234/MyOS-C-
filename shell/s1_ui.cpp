// ===========================================================================
//  SHELL s1: UI-ТУЛКИТ «AQUA» — сглаженные скругления, тени, настоящее стекло,
//            иконки приложений, символы меню-бара и тема (тёмная/светлая)
// ===========================================================================
//  Модуль добавлен к v3.2 ради «маковского» вида оболочки. Правила те же, что
//  во всём ядре: НИ ОДНОГО floating point (FPU отключён флагом -mno-80387),
//  поэтому вся гладкость сделана на целых числах:
//
//    * сглаживание (AA) скруглённых углов — по покрытию пикселя дугой,
//      геометрия в «полупикселях», т.е. точно посередине между пикселями;
//    * тени окон — несколько концентрических контуров с квадратичным спадом;
//    * «стекло» — честное смешивание с прочитанным из видеопамяти цветом
//      (раньше полупрозрачность изображалась дизерингом Байера — на macOS
//      так не делают, и картинка выглядела «рябой»).
//
//  Порядок включения: сразу после s0 (нужны put_pixel/read_pixel/fill_rect).
// ===========================================================================

// ---------------------------------------------------------------------------
//  Мелкая целочисленная математика
// ---------------------------------------------------------------------------
int iclamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

// Сколько байт занимает пиксель экрана (3 или 4) — короткий доступ к FB_BPP.
int FBB() { return FB_BPP / 8; }

// Целочисленный квадратный корень (метод Ньютона). Без sqrtf(): FPU нет.
int isqrt(int v) {
    if (v <= 0) return 0;
    int x = v, y = (x + 1) / 2;
    while (y < x) { x = y; y = (x + v / x) / 2; }
    return x;
}

// Смешать два готовых RGB-цвета: a = доля src (0..256). (mix_col — в s0.)
unsigned int blend_rgb(unsigned int dst, unsigned int src, int a) {
    return mix_col(dst, src, a);
}

// Нарисовать одну точку «наполовину прозрачным» цветом (a = 0..256).
void blend_px(int x, int y, unsigned int c, int a) {
    if (a <= 0) return;
    if (x < 0 || x >= W || y < 0 || y >= H) return;
    if (FBB() == 32) {                     // быстрый путь: 4 байта на пиксель
        a = iclamp(a, 0, 256);
        if (a >= 256) { put_pixel(x, y, c); return; }
        unsigned int ca = encode_color(c);
        unsigned int* p = (unsigned int*)(fb + y * FB_PITCH) + x;
        unsigned int old = *p, ia = (unsigned int)(256 - a);
        unsigned int rr = ((ca & 0x00FF0000) * a + (old & 0x00FF0000) * ia) >> 8;
        unsigned int gg = ((ca & 0x0000FF00) * a + (old & 0x0000FF00) * ia) >> 8;
        unsigned int bb = ((ca & 0x000000FF) * a + (old & 0x000000FF) * ia) >> 8;
        *p = (rr & 0x00FF0000) | (gg & 0x0000FF00) | (bb & 0x000000FF);
        return;
    }
    // общий путь (24 бита и необычные маски каналов): читаем и мешаем по каналам
    unsigned int old = read_pixel(x, y);
    put_pixel(x, y, blend_rgb(old, c, a));
}

// Горизонтальный отрезок «наполовину прозрачным» цветом.
void blend_span(int x, int y, int w, unsigned int c, int a) {
    if (a <= 0 || w <= 0) return;
    if (a >= 256) { fill_rect(x, y, w, 1, c); return; }
    int x0 = x < 0 ? 0 : x, x1 = x + w; if (x1 > W) x1 = W;
    if (y < 0 || y >= H || x0 >= x1) return;
    if (FBB() == 32) {
        unsigned int ca = encode_color(c), ia = (unsigned int)(256 - a);
        unsigned int* p = (unsigned int*)(fb + y * FB_PITCH) + x0;
        unsigned int cr = ca & 0x00FF0000, cg = ca & 0x0000FF00, cb = ca & 0x000000FF;
        for (int i = x0; i < x1; i++, p++) {
            unsigned int old = *p;
            unsigned int rr = ((cr * a + (old & 0x00FF0000) * ia) >> 8) & 0x00FF0000;
            unsigned int gg = ((cg * a + (old & 0x0000FF00) * ia) >> 8) & 0x0000FF00;
            unsigned int bb = ((cb * a + (old & 0x000000FF) * ia) >> 8) & 0x000000FF;
            *p = rr | gg | bb;
        }
        return;
    }
    for (int i = x0; i < x1; i++) {
        unsigned int old = read_pixel(i, y);
        put_pixel(i, y, blend_rgb(old, c, a));
    }
}

// Байт на пиксель — короткий доступ к глобалу FB_BPP из s0.

// ---------------------------------------------------------------------------
//  СКРУГЛЁННЫЕ ПРЯМОУГОЛЬНИКИ СО СГЛАЖИВАНИЕМ
// ---------------------------------------------------------------------------
//  Геометрия: радиус r в пикселях. Работаем в «полупикселях» (единица = 0.5px),
//  поэтому R = 2r, а длина вектора меряется до центра дуги без дробей.
//
//  corner_inset8(j, h, r) — на сколько ПИКСЕЛЕЙ (в формате 8.8) отступает
//  край строки j от прямоугольника из-за скругления. 0 — строка прямая.
int corner_inset8(int j, int h, int r) {
    if (r <= 0) return 0;
    int R = r * 2;
    int dyh;
    if (j < r)                dyh = R - 1 - 2 * j;             // верхние дуги
    else if (j >= h - r)      dyh = 2 * (j - (h - r)) + 1;     // нижние дуги
    else                      return 0;                        // средняя часть
    if (dyh > R) dyh = R;
    int dxh = isqrt(R * R - dyh * dyh);                        // ширина дуги
    int ins8 = (R - dxh) * 128;                                // в пиксели (8.8)
    return iclamp(ins8, 0, (r * 256));
}

// Залить скруглённый прямоугольник с вертикальным ГРАДИЕНТОМ и плотностью.
// ct — цвет верхней строки, cb — нижней, alpha — 0..256 (256 = непрозрачно).
void fill_round_grad(int x, int y, int w, int h, int r,
                     unsigned int ct, unsigned int cb, int alpha) {
    if (w <= 0 || h <= 0) return;
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    alpha = iclamp(alpha, 0, 256);
    for (int j = 0; j < h; j++) {
        int py = y + j;
        if (py < 0 || py >= H) continue;
        unsigned int c = (h > 1) ? mix_col(ct, cb, j * 256 / (h - 1)) : ct;
        int l8 = corner_inset8(j, h, r);          // 8.8 слева
        int r8 = (w << 8) - l8;                   // 8.8 справа
        int x0 = l8 >> 8, x1 = r8 >> 8;
        if (x1 <= x0) continue;
        // частичные (сглаженные) пиксели по краям
        int al = (256 - (l8 & 255)); if (al > 256) al = 256;
        int ar = (r8 & 255);
        al = (al * alpha) >> 8;
        ar = (ar * alpha) >> 8;
        if (al > 0) blend_px(x + x0, py, c, al);
        if (ar > 0) blend_px(x + x1, py, c, ar);
        // сплошная середина
        int mw = x1 - x0 - 1;
        if (mw > 0) blend_span(x + x0 + 1, py, mw, c, alpha);
    }
}

void fill_round(int x, int y, int w, int h, int r, unsigned int c) {
    fill_round_grad(x, y, w, h, r, c, c, 256);
}
void fill_round_a(int x, int y, int w, int h, int r, unsigned int c, int a) {
    fill_round_grad(x, y, w, h, r, c, c, a);
}

// Контур скруглённого прямоугольника толщиной 1 пиксель, со сглаживанием.
void stroke_round(int x, int y, int w, int h, int r, unsigned int c, int alpha) {
    if (w <= 0 || h <= 0) return;
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    alpha = iclamp(alpha, 0, 256);
    for (int j = 0; j < h; j++) {
        int py = y + j;
        int l8 = corner_inset8(j, h, r);
        int r8 = (w << 8) - l8;
        int lx = x + (l8 >> 8), rx = x + ((r8 - 1) >> 8);
        if (j == 0 || j == h - 1) {               // верх/низ — сплошная линия
            int a2 = ((256 - (l8 & 255)) * alpha) >> 8;
            blend_px(lx, py, c, a2);
            a2 = (((r8 - 1) & 255) * alpha) >> 8;
            blend_px(rx, py, c, a2);
            int mw = rx - lx - 1;
            if (mw > 0) blend_span(lx + 1, py, mw, c, alpha);
            continue;
        }
        // боковые «стенки»: сглаживание по дробной части отступа
        int fl = l8 & 255;                       // 0..255 — насколько пиксель срезан
        blend_px(lx - 1, py, c, ((256 - fl) * alpha) >> 8);   // «недорезанный» пиксель
        blend_px(lx, py, c, (fl * alpha) >> 8);               // «перерезанный»
        int fr = (r8 - 1) & 255;
        blend_px(rx + 1, py, c, (fr * alpha) >> 8);
        blend_px(rx, py, c, ((256 - fr) * alpha) >> 8);
    }
}

// Мягкая тень вокруг прямоугольника: несколько контуров с квадратичным спадом.
// spread — на сколько пикселей «расплывается», strength — плотность у окна.
void shadow_round(int x, int y, int w, int h, int r, int spread, int strength) {
    if (spread < 1) return;
    for (int k = spread; k >= 1; k--) {
        int t = spread - k + 1;                  // 1..spread (ближе к окну — плотнее)
        int a = strength * t * t / (spread * spread);
        if (a <= 0) continue;
        int dy = (k * 2) / spread;               // тень чуть смещена вниз
        stroke_round(x - k, y - k + dy, w + 2 * k, h + 2 * k, r + k, 0x000000, a);
    }
}

// Круг с плотностью (для мелких точек и кнопок) — без сглаживания: радиусы
// у нас маленькие, а таких точек много (светофор, индикаторы дока).
void fill_circle_a(int cx, int cy, int r, unsigned int c, int a) {
    int r2 = r * r;
    for (int dy = -r; dy <= r; dy++)
        for (int dx = -r; dx <= r; dx++)
            if (dx * dx + dy * dy <= r2) blend_px(cx + dx, cy + dy, c, a);
}

// Верхняя половина окружности (для значка Wi-Fi): дуги без нижней части.
void circle_top(int cx, int cy, int r, unsigned int c, int a) {
    for (int dy = -r; dy <= 0; dy++)
        for (int dx = -r; dx <= r; dx++) {
            int d2 = dx * dx + dy * dy;
            if (d2 >= r * r - r && d2 <= r * r + r) blend_px(cx + dx, cy + dy, c, a);
        }
}

// ---------------------------------------------------------------------------
//  ТЕКСТ
// ---------------------------------------------------------------------------
int text_w(const char* s) { int n = 0; while (s[n]) n++; return n * 8; }

void draw_text_a(int x, int y, const char* s, unsigned int c, int a) {
    if (a >= 256) { draw_text(x, y, s, c); return; }
    while (*s) {
        char ch = *s;
        if (ch >= 32 && ch <= 126) {
            const unsigned char* g = font8x16[ch - 32];
            for (int row = 0; row < 16; row++) {
                unsigned char bits = g[row];
                if (!bits) continue;
                for (int col = 0; col < 8; col++)
                    if (bits & (0x80 >> col)) blend_px(x + col, y + row, c, a);
            }
        }
        x += 8; s++;
    }
}

// «Жирный» текст: две копии со сдвигом на пиксель (в растровом шрифте 8x16
// настоящего Bold нет, а для меню-бара и заголовков разница заметна).
void draw_text_bold(int x, int y, const char* s, unsigned int c) {
    draw_text(x, y, s, c);
    draw_text(x + 1, y, s, c);
}
void draw_text_bold_a(int x, int y, const char* s, unsigned int c, int a) {
    draw_text_a(x, y, s, c, a);
    draw_text_a(x + 1, y, s, c, a);
}

// Текст с мягкой тенью (меню-бар поверх светлых обоев — иначе не читается).
void draw_text_shadow(int x, int y, const char* s, unsigned int c, unsigned int sh) {
    draw_text_a(x, y + 1, s, sh, 110);
    draw_text(x, y, s, c);
}

// Символ из «пиксельной карты» (ASCII-арт: '#' — точка). Удобно для значков,
// у которых важна форма: стрелки, галочки, крестик светофора.
void draw_glyph(int x, int y, const char* const* g, int gw, int gh,
                unsigned int c, int a) {
    for (int j = 0; j < gh; j++)
        for (int i = 0; i < gw; i++)
            if (g[j][i] == '#') blend_px(x + i, y + j, c, a);
}

// ---------------------------------------------------------------------------
//  ТЕМА: тёмная и светлая (палитры macOS)
// ---------------------------------------------------------------------------
//  Все цвета интерфейса — ГЛОБАЛЬНЫЕ переменные, а не константы: смена темы
//  пересчитывает их и перерисовывает кадр. Приложения читают те же имена
//  (TEXT_COL, C_WIN_BODY), поэтому им не нужно ничего знать про тему.
bool ui_dark = true;                 // текущая тема: 1 — тёмная (по умолчанию)

unsigned int TEXT_COL     = 0xE8E9EC;   // основной текст в окнах
unsigned int C_WIN_BODY   = 0x1E1F22;   // тело окна (низ градиента)
unsigned int C_WIN_BODY_T = 0x232428;   // тело окна (верх градиента)
unsigned int C_WIN_TITLE  = 0x2E2F34;   // шапка активного окна
unsigned int C_WIN_TITLE2 = 0x26272B;   // шапка неактивного окна
unsigned int C_WIN_EDGE   = 0x3C3D42;   // контур окна
unsigned int C_WIN_SEP    = 0x3A3B40;   // разделители внутри окна
unsigned int C_WIN_DIM    = 0x9A9CA3;   // второстепенный текст
unsigned int C_SEL        = 0x0A84FF;   // системный синий (выделение)
unsigned int C_SEL_TEXT   = 0xFFFFFF;
unsigned int C_MBAR_TINT  = 0x1A1A1E;   // подложка меню-бара
unsigned int C_MBAR_TEXT  = 0xEDEDEF;
unsigned int C_MENU_BG    = 0x2A2B2F;   // фон выпадающего меню
unsigned int C_MENU_HOT   = 0x0A84FF;   // подсветка пункта меню
unsigned int C_DOCK_TINT  = 0x2A2B30;   // подложка дока
unsigned int C_PANEL      = 0x2C2D31;   // панели (Control Center, виджеты)
unsigned int C_TOOLTIP    = 0x3A3B40;   // подсказки над доком
unsigned int C_TITLE_TEXT = 0xE8E9EC;   // текст заголовка окна

// Пересчитать палитру под текущую тему (вызывается при старте и по переключателю).
void apply_theme() {
    if (ui_dark) {
        TEXT_COL = 0xE8E9EC;
        C_WIN_BODY = 0x1A1B1F; C_WIN_BODY_T = 0x212227;
        C_WIN_TITLE = 0x303138; C_WIN_TITLE2 = 0x25262B;
        C_WIN_EDGE = 0x40414A; C_WIN_SEP = 0x36373D; C_WIN_DIM = 0x9A9CA3;
        C_SEL = 0x0A84FF; C_SEL_TEXT = 0xFFFFFF;
        C_MBAR_TINT = 0x14151A; C_MBAR_TEXT = 0xEDEDEF;
        C_MENU_BG = 0x2B2C31; C_MENU_HOT = 0x0A84FF;
        C_DOCK_TINT = 0x2A2B30;
        C_PANEL = 0x2C2D32; C_TOOLTIP = 0x3A3B41;
        C_TITLE_TEXT = 0xE8E9EC;
    } else {
        TEXT_COL = 0x1D1D1F;
        C_WIN_BODY = 0xF6F6F8; C_WIN_BODY_T = 0xFFFFFF;
        C_WIN_TITLE = 0xE4E4E7; C_WIN_TITLE2 = 0xEDEDF0;
        C_WIN_EDGE = 0xC9C9CF; C_WIN_SEP = 0xDCDCE1; C_WIN_DIM = 0x77777E;
        C_SEL = 0x007AFF; C_SEL_TEXT = 0xFFFFFF;
        C_MBAR_TINT = 0xF2F2F5; C_MBAR_TEXT = 0x1D1D1F;
        C_MENU_BG = 0xFBFBFD; C_MENU_HOT = 0x007AFF;
        C_DOCK_TINT = 0xEDEDF0;
        C_PANEL = 0xF7F7FA; C_TOOLTIP = 0x45454C;
        C_TITLE_TEXT = 0x1D1D1F;
    }
}

// ---------------------------------------------------------------------------
//  ИКОНКИ ПРИЛОЖЕНИЙ
// ---------------------------------------------------------------------------
//  Иконка macOS — это «сквиркл» (скруглённый квадрат с большим радиусом),
//  1-битная картинка приложения по центру и подсветка сверху.
//  Картинки приложений (20x20) лежат в icons_data.inc — их и масштабируем.
void draw_icon_scaled(int x, int y, const unsigned char* bmp, int dst,
                      unsigned int color, int alpha) {
    if (dst <= 0) return;
    for (int j = 0; j < dst; j++) {
        int sj = j * 20 / dst;
        unsigned int bits = ((unsigned int)bmp[sj * 3] << 16) |
                            ((unsigned int)bmp[sj * 3 + 1] << 8) |
                             (unsigned int)bmp[sj * 3 + 2];
        if (!bits) continue;
        for (int i = 0; i < dst; i++) {
            int si = i * 20 / dst;
            if (bits & (0x800000u >> si))
                blend_px(x + i, y + j, color, alpha);
        }
    }
}

// Цвета «плиток» приложений: верх и низ градиента (как у иконок macOS).
static const unsigned int ICON_TOP[8] = {
    0x5B5F6B, 0x59A8F7, 0xB6BCC8, 0x4CD97F, 0x8F8DF2, 0xFFC155, 0x74D6F8, 0x9AA0AC
};
static const unsigned int ICON_BOT[8] = {
    0x24262C, 0x1A6FD0, 0x60656F, 0x1FA24E, 0x5B58C8, 0xE8801A, 0x2E9FD6, 0x5A6068
};

// Нарисовать иконку приложения app (0..DOCK_N-1) размером sz в точке (x,y).
// hover: 1 — курсор на иконке (чуть светлее), 0 — обычная.
void draw_app_icon(int app, int x, int y, int sz, bool hover) {
    if (sz < 12) sz = 12;
    int a = app & 7;
    int r = sz * 23 / 100;                        // радиус «сквиркла»
    if (r < 3) r = 3;
    int top = hover ? mix_col(ICON_TOP[a], 0xFFFFFF, 34) : ICON_TOP[a];
    int bot = hover ? mix_col(ICON_BOT[a], 0xFFFFFF, 18) : ICON_BOT[a];
    shadow_round(x, y + 1, sz, sz, r, 3, 34);     // мягкая тень под иконкой
    fill_round_grad(x, y, sz, sz, r, top, bot, 256);
    // глянцевый блик по верхней части (как у иконок macOS)
    fill_round_grad(x + 1, y + 1, sz - 2, sz / 2, r - 1,
                    mix_col(top, 0xFFFFFF, 90), mix_col(top, 0xFFFFFF, 10), 90);
    stroke_round(x, y, sz, sz, r, mix_col(bot, 0x000000, 90), 130);
    // картинка приложения по центру
    int gs = sz * 58 / 100;
    draw_icon_scaled(x + (sz - gs) / 2, y + (sz - gs) / 2, dock_icons[app], gs,
                     (a == 2) ? 0x2B2E34 : 0xFFFFFF, 240);
}

// Иконка «Корзина» — рисуем сами (нет 1-битной картинки в icons_data.inc).
void draw_trash_icon(int x, int y, int sz, bool hover) {
    int r = sz * 23 / 100;
    int top = hover ? 0xC9CFDA : 0xA8AEB8;
    int bot = hover ? 0x8C929C : 0x70767F;
    shadow_round(x, y + 1, sz, sz, r, 3, 30);
    fill_round_grad(x, y, sz, sz, r, top, bot, 210);
    stroke_round(x, y, sz, sz, r, 0x000000, 80);
    // сама корзина: крышка, корпус, ручка
    int cx = x + sz / 2, t = y + sz * 30 / 100;
    int bw = sz * 40 / 100, bh = sz * 34 / 100;
    unsigned int c = 0x3C4148;
    fill_rect(cx - bw / 2, t, bw, 2, c);                     // крышка
    fill_rect(cx - bw / 2 - 2, t + 3, bw + 4, bh, c);        // корпус
    fill_rect(cx - bw / 2 - 2, t + 3, bw + 4, 1, mix_col(c, 0xFFFFFF, 90));
    fill_rect(cx - 3, t - 3, 6, 2, c);                        // ручка
    for (int k = 0; k < 3; k++)                               // рёбра
        fill_rect(cx - bw / 2 + 3 + k * (bw / 3), t + 6, 1, bh - 6, mix_col(c, 0xFFFFFF, 110));
}

// ---------------------------------------------------------------------------
//  СТЕКЛО (glassmorphism) — тонированный цвет поверх того, что под ним
// ---------------------------------------------------------------------------
//  fill_glass кладёт цвет заданной плотности. Сглаженные края дают
//  fill_round_a / fill_round_grad (см. выше): в отличие от старой версии,
//  где полупрозрачность изображалась дизерингом Байера, здесь настоящее
//  смешивание с прочитанным пикселем — так и выглядят панели в macOS.
//  comp_full = true означает «идёт полный кадр»: стёкла рисуются
//  полупрозрачными. При точечной перерисовке — плотно, чтобы не наслаивать.
bool comp_full = false;

void fill_glass(int x, int y, int w, int h, int r, unsigned int color, int alpha) {
    int a = comp_full ? alpha : 256;
    fill_round_a(x, y, w, h, r, color, a);
}

// Точечная полупрозрачность без чтения фона (быстрый дизеринг 4x4). Нужна
// для тонких бликов и линий поверх уже готового стекла.
const unsigned char bayer4[16] = {
     0,  8,  2, 10,
    12,  4, 14,  6,
     3, 11,  1,  9,
    15,  7, 13,  5
};
void dither_over(int x, int y, int w, int h, unsigned int color, int dens) {
    for (int j = y; j < y + h; j++)
        for (int i = x; i < x + w; i++)
            if (i >= 0 && i < W && j >= 0 && j < H)
                if (bayer4[(j & 3) * 4 + (i & 3)] < dens)
                    put_pixel(i, j, color);
}

// Сглаженный контур скруглённого прямоугольника (тонкая рамка).
void draw_round_rect(int x, int y, int w, int h, int r, unsigned int color) {
    stroke_round(x, y, w, h, r, color, 256);
}

// Вертикальный градиент (сплошной прямоугольник).
void fill_vgrad(int x, int y, int w, int h, unsigned int ct, unsigned int cb) {
    for (int j = 0; j < h; j++) {
        unsigned int c = (h > 1) ? mix_col(ct, cb, j * 256 / (h - 1)) : ct;
        fill_rect(x, y + j, w, 1, c);
    }
}

// Радиальный блеск «линзы» на стекле — мягкий эллипс сверху (навигация в
// Control Center и строка поиска Spotlight выглядят «стеклянными»).
void gloss(int x, int y, int w, int h, int a) {
    int cx = x + w / 2, cy = y + h / 3;
    for (int j = 0; j < h / 2; j++)
        for (int i = 0; i < w; i++) {
            int dx = (i - w / 2) * 2, dy = (j - h / 3) * 2;
            int d2 = dx * dx + dy * dy;
            int rr = (w * w) / 2 + 1;
            if (d2 >= rr) continue;
            int k = 256 - d2 * 256 / rr;
            blend_px(x + i, y + j, 0xFFFFFF, (k * a) >> 10);
        }
    (void)cx; (void)cy;
}
