// ===========================================================================
//  SHELL s5: desktop - clock, menubar, dock, About
// ===========================================================================
//  MyOS v2.0 module. Split mechanically from kernel.cpp v1.2
//  (git a81d4f4) and then reworked: see README section 11.

// ===========================================================================
//  РАБОЧИЙ СТОЛ: меню-бар, живые часы и док приложений
//  (Terminal, Files, Settings, Tasks, About — окна см. ниже)
// ===========================================================================
//  Тёмная тема «Night Ridge» (по концепту): меню-бар — матовое стекло над
//  обоями, слева логотип MyOS, справа — индикатор батареи и живые часы.
//  Док снизу: стеклянная панель с иконками-картинками. Стрелки Left/Right —
//  выбор, Enter — запуск, точки под иконкой — «приложение запущено».

int last_clock_sec = -1;                 // секунда, показанная в прошлый раз

// Восстановить «стекло меню-бара» над обоями в области (частичная
// перерисовка часов): обои в этой зоне считаются той же чистой функцией,
// что рисует весь экран, затем поверх ложится цвет бара той же плотности.
static void erase_mbar(int x, int y, int w, int h) {
    for (int j = 0; j < h; j++) {
        int yy = y + j; if (yy < 0 || yy >= MB_H) continue;
        for (int i = 0; i < w; i++) {
            int xx = x + i; if (xx < 0 || xx >= W) continue;
            put_pixel(xx, yy, mix_col(wallpaper_pixel(xx, yy), C_GLASS_MBAR, GLASS_MBAR));
        }
    }
}

// Нарисовать часы в правом углу меню-бара: «18:42» или «6:42PM».
// Формат переключается в окне Settings (переменная clock_24h).
int clock_24h = 1;                       // 1 = 24 часа, 0 = 12 часов с AM/PM
void draw_clock() {
    unsigned int t = day_secs();         // v2.0: время даёт ядро (SYS_TIME)
    int h = (int)((t / 3600) % 24);      // часы
    int m = (int)((t / 60) % 60);        // минуты
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
    erase_mbar(W - 70, 0, 70, MB_H);                    // стереть старые цифры
    draw_text(W - 62, 4, buf, C_WHITE);                 // 7 знаков максимум
}

// Вызывается в главном цикле: CMOS оттикал новую секунду — обновить часы
void clock_tick() {
    int s = (int)(day_secs() % 60);      // текущая секунда суток
    if (s == last_clock_sec) return;     // та же секунда — экран не трогаем
    last_clock_sec = s;
    cursor_hide();                       // часы могут попасть под курсор
    draw_clock();
    cursor_show();
}

// Индикатор батареи: тонкий корпус с контактом и зарядом примерно в 60%.
// Статический значок — рисуется только в полном кадре меню-бара.
static void draw_battery(int bx, int cy) {
    draw_round_rect(bx, cy - 5, 18, 10, 3, C_TEXT_DIM);  // корпус
    fill_rect(bx + 2, cy - 3, 10, 6, C_TEXT_DIM);        // уровень заряда
    fill_rect(bx + 19, cy - 2, 2, 4, C_TEXT_DIM);        // контакт
}

// Меню-бар: матовое стекло во всю ширину + логотип + батарея + часы.
// Меню-бар рисуется ПЕРВЫМ в полном кадре — его «стекло» смешивается с
// свежими обоями (comp_full уже включён на этот момент).
void draw_menubar() {
    fill_glass(0, 0, W, MB_H, 0, C_GLASS_MBAR, GLASS_MBAR);   // матовое стекло
    dither_over(0, MB_H - 1, W, 1, C_WHITE, 3);               // тонкий блик снизу
    // логотип: стеклянная плашка с точкой-«целью» цвета акцента
    fill_glass(8, 4, 16, 16, 5, C_GLASS_ICON, GLASS_ICON);
    draw_round_rect(8, 4, 16, 16, 5, C_ACCENT);
    fill_circle(16, 12, 3, C_ACCENT);
    draw_text(30, 4, "MyOS", C_WHITE);
    draw_battery(W - 92, 12);                                 // справа — заряд
    draw_clock();                                             // и часы
}

// --- Док (панель приложений внизу экрана) ---
const int DOCK_N = 7;                            // сколько приложений в доке
// Размеры зависят от экрана: их считает init_desktop_layout() (ниже).
// Крупный экран — большие иконки 52px с битовой картиной в 2 раза больше,
// узкий — прежние 28px без масштаба, чтобы док влезал в 320 столбцов.
int ICON_SZ = 28, ICON_GAP = 10, DOCK_PAD = 8;   // текущая геометрия
int DOCK_ICON_SCALE = 1;                         // масштаб битовый карты иконки
int DOCK_R = 12;                                 // скругление плашки дока
int DOCK_W = 250, DOCK_H = 46;                   // габариты плашки
int DOCK_X = 35, DOCK_Y = 150;                   // по центру (обновит init_desktop_layout)
int dock_sel = 0;                                // какая иконка выбрана
const char* dock_names[DOCK_N] = { "Terminal", "Files", "Settings", "Tasks", "Code", "Game", "About" };

// Одна иконка: чёрный фон, белая рамка, символ приложения.
// Выбранная — дополнительно в двойной рамке.
// --- Иконки дока: картинки 20x20 пикселей (сгенерировал icons_gen.ps1) ---
#include "../icons_data.inc"

// Нарисовать иконку 20x20 из битовой карты с масштабом s (1 = как есть,
// 2 = 40x40 для больших доков): 3 байта на строку, старший бит — левый пиксель.
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

void draw_icon20s(int x, int y, const unsigned char* bmp, int s, unsigned int color) {
    if (s <= 1) { draw_icon20(x, y, bmp, color); return; }
    for (int j = 0; j < 20; j++) {
        unsigned int bits = ((unsigned int)bmp[j * 3] << 16) |
                            ((unsigned int)bmp[j * 3 + 1] << 8) |
                             (unsigned int)bmp[j * 3 + 2];
        if (!bits) continue;
        for (int i = 0; i < 20; i++) {
            if (!(bits & (0x800000u >> i))) continue;
            // масштабируем квадратом s x s — чисто и быстро
            for (int jj = 0; jj < s; jj++)
                for (int ii = 0; ii < s; ii++)
                    put_pixel(x + i * s + ii, y + j * s + jj, color);
        }
    }
}

// Иконка дока: стеклянная карточка ПОД иконкой, картинка, точка «запущено».
// Выбранная (клавиатура/наведение) — с обводкой цвета акцента.
void draw_dock_icon(int i) {
    int ix = DOCK_X + DOCK_PAD + i * (ICON_SZ + ICON_GAP);
    int iy = DOCK_Y + DOCK_PAD;
    bool hot = (i == dock_sel);
    fill_glass(ix, iy, ICON_SZ, ICON_SZ, ICON_SZ / 2 - 1,
               hot ? C_GLASS_ICON_HV : C_GLASS_ICON,
               hot ? GLASS_ICON_HV : GLASS_ICON);               // подложка
    if (hot) draw_round_rect(ix - 4, iy - 4, ICON_SZ + 8, ICON_SZ + 8,
                             ICON_SZ / 2 - 1, C_ACCENT);        // выделение
    int gs = 20 * DOCK_ICON_SCALE;
    draw_icon20s(ix + (ICON_SZ - gs) / 2, iy + (ICON_SZ - gs) / 2,
                 dock_icons[i], DOCK_ICON_SCALE, C_WHITE);      // картинка
    if (app_is_open(i))                                         // «запущено»
        fill_circle(ix + ICON_SZ / 2, iy + ICON_SZ + 4, 2, C_WHITE);
}

// Подпись выбранного приложения — стеклянная «таблетка» над доком
void draw_dock_label() {
    erase_to_wallpaper(0, DOCK_Y - 22, W, 20);             // стереть старую подпись
    const char* s = dock_names[dock_sel];
    int n = 0; while (s[n]) n++;                           // длина названия
    int wpx = n * 8 + 24;                                  // ширина таблетки
    int lx = (W - wpx) / 2;
    fill_glass(lx, DOCK_Y - 20, wpx, 16, 8, C_GLASS_MBAR, GLASS_MBAR + 30);
    draw_text(lx + 12, DOCK_Y - 18, s, C_WHITE);           // центрировано
}

// Весь док: стеклянная плашка + иконки + подпись
void draw_dock() {
    fill_glass(DOCK_X, DOCK_Y, DOCK_W, DOCK_H, DOCK_R, C_GLASS_DOCK, GLASS_DOCK);
    // мягкая рамка и блик по верху — стеклянная пластина
    draw_round_rect(DOCK_X, DOCK_Y, DOCK_W, DOCK_H, DOCK_R, C_EDGE);
    dither_over(DOCK_X + DOCK_R, DOCK_Y + 1, DOCK_W - 2 * DOCK_R, 1, C_WHITE, 4);
    for (int i = 0; i < DOCK_N; i++) draw_dock_icon(i);
    draw_dock_label();
}

// Перерисовать ТОЛЬКО док (плашку и подпись). Окна до дока не достают — их
// положение ограничено сверху, поэтому под доком всегда обои, и стереть его
// область «до обоев» безопасно. Нужно для подсветки иконок: полный кадр на
// каждое движение мыши не нужен — из-за него экран мерцал и тормозил.
void draw_dock_area() {
    erase_to_wallpaper(0, DOCK_Y - 24, W, H - (DOCK_Y - 24));
    comp_full = true;                        // док рисуем «стеком», как в кадре
    draw_dock();
    comp_full = false;
}

// Рабочий стол целиком, с нуля: обои + меню-бар + окна (по слоям) + док.
// Тело — в конце файла (draw_desktop_real), когда известны все окна.

// --- Окно «About» (справка о системе) ---
bool about_active = false;               // открыто ли окно About

// Геометрия окна About — по центру экрана (обновит init_desktop_layout)
const int ABT_W = 210, ABT_H = 94;
int ABT_X = (320 - ABT_W) / 2;
int ABT_Y = 52;

// Пересчитать позиции дока и окна About под ТЕКУЩИЙ экран.
// Док адаптивный: чем шире экран, тем крупнее иконки (52px с масштабом 2
// битовый карты наиближжем экране; на 320-колоночном — прежние 28px).
// Вызывается после init_layout() — когда W/H уже известны точно.
void init_desktop_layout() {
    if      (W >= 900) { ICON_SZ = 52; DOCK_ICON_SCALE = 2; ICON_GAP = 16; DOCK_PAD = 14; DOCK_R = 18; }
    else if (W >= 640) { ICON_SZ = 44; DOCK_ICON_SCALE = 2; ICON_GAP = 12; DOCK_PAD = 12; DOCK_R = 14; }
    else if (W >= 460) { ICON_SZ = 34; DOCK_ICON_SCALE = 1; ICON_GAP = 10; DOCK_PAD = 10; DOCK_R = 14; }
    else               { ICON_SZ = 28; DOCK_ICON_SCALE = 1; ICON_GAP = 8;  DOCK_PAD = 7;  DOCK_R = 12; }
    DOCK_W = 2 * DOCK_PAD + DOCK_N * ICON_SZ + (DOCK_N - 1) * ICON_GAP;
    DOCK_H = DOCK_PAD * 2 + ICON_SZ + 7;                   // + место под точку «запущено»
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
    draw_text(tx, ty, "MyOS v2.0 (Night Ridge)", TEXT_COL);

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
