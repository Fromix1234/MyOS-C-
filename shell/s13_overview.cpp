// ===========================================================================
//  SHELL s13: ACTIVITIES — полноэкранный обзор приложений (клавиша F2)
// ===========================================================================
//  Реализация второго концепта: поверх рабочего стола открывается «обзор»,
//  где все приложения видны крупными плитками, а сверху — строка поиска.
//    - поиск: если начать печатать, сетка фильтруется по имени приложения;
//    - стрелки двигают выбор, Enter запускает приложение;
//    - щелчок мыши по плитке — запуск, мимо плиток — вернуться на стол;
//    - Esc / F2 — вернуться на рабочий стол; F3 — сразу консоль.
//  Плитки — стеклянные (та же технология, что окна и док): сквозь них видно
//  затемнённые обои. Идея рисовать НАД deskом, а не вместо: обои не «прыгают».
// ===========================================================================

int  ov_sel = 0;              // выбранная ПЛИТКА (индекс в отфильтрованном списке)
char ov_query[24];            // текст поиска
int  ov_qlen = 0;

// Список приложений после фильтра: номера приложений (= номера иконок дока)
static int ov_list[DOCK_N];
static int ov_n = 0;

// Геометрия плиток (заполняет draw_overview, читает overview_mouse)
static int ov_tx[DOCK_N], ov_ty[DOCK_N];
static int ov_tw = 150, ov_th = 66;
static int ov_cols = 3, ov_rows = 1;
static int ov_sy = 0, ov_sw = 0, ov_sh = 30;    // строка поиска

// Фильтр: имя приложения содержит запрос целиком (регистр игнорируем).
// Это поиск «содержит», а не «начинается с» — так прощают опечатку в начале.
static bool ov_match(int app) {
    if (ov_qlen == 0) return true;
    const char* n = dock_names[app];
    int in = 0;                                    // сколько уже совпало подряд
    for (int i = 0; n[i]; i++) {
        char c = n[i]; if (c >= 'A' && c <= 'Z') c = (char)(c + 32);      // lower
        char q = ov_query[in]; if (q >= 'A' && q <= 'Z') q = (char)(q + 32);
        if (c == q) {
            if (++in == ov_qlen) return true;      // весь запрос сошёлся
        } else {
            in = 0;
        }
    }
    return false;
}

static void ov_recalc() {
    ov_n = 0;
    for (int a = 0; a < DOCK_N; a++)
        if (ov_match(a)) ov_list[ov_n++] = a;
    if (ov_sel >= ov_n) ov_sel = ov_n - 1;
    if (ov_sel < 0) ov_sel = 0;
}

static void ov_geo() {           // строка поиска: ширина по экрану
    ov_sw = (W >= 1000) ? 460 : (W >= 640) ? 360 : W - 60;
    ov_sh = 30;
    ov_sy = MB_H + 16;
}

// Нарисовать обзор целиком: обои + меню-бар + затемнение + поиск + плитки.
// Это полноценный кадр (comp_full включён) — стекло смешивается честно.
void draw_overview() {
    ov_geo();
    comp_full = true;
    fill_wallpaper();                                    // свежие обои под кадром
    draw_menubar();
    fill_glass(0, MB_H, W, H - MB_H, 0, C_BLACK, 150);   // «занавес» поверх обоев

    // --- строка поиска (стеклянная «пилюля» с лупой) ---
    int sx = (W - ov_sw) / 2;
    fill_glass(sx, ov_sy, ov_sw, ov_sh, 15, C_GLASS_TILE, GLASS_TILE);
    draw_round_rect(sx, ov_sy, ov_sw, ov_sh, 15, ov_qlen ? C_ACCENT : C_EDGE);
    draw_circle(sx + 18, ov_sy + 14, 4, C_TEXT_DIM);     // лупа
    draw_line (sx + 21, ov_sy + 17, sx + 25, ov_sy + 21, C_TEXT_DIM);
    if (ov_qlen) draw_text(sx + 34, ov_sy + 7, ov_query, C_WHITE);
    else         draw_text(sx + 34, ov_sy + 7, "Search apps, files, settings...", 0x6E7488);

    // --- сетка плиток приложений ---
    ov_recalc();
    ov_cols = (W >= 1100) ? 4 : (W >= 820) ? 3 : 2;
    ov_rows = (ov_n + ov_cols - 1) / ov_cols;
    int tw = (W - 32 - (ov_cols - 1) * 24) / ov_cols;
    if (tw > 220) tw = 220;
    if (tw < 120) tw = 120;
    ov_tw = tw;
    ov_th = (tw >= 170) ? 108 : 66;
    int gx = (W - (ov_cols * tw + (ov_cols - 1) * 24)) / 2;
    int gy = ov_sy + ov_sh + 44;
    for (int t = 0; t < ov_n; t++) {
        int cix = t % ov_cols, ciy = t / ov_cols;
        int x = gx + cix * (tw + 24), y = gy + ciy * (ov_th + 24);
        ov_tx[t] = x; ov_ty[t] = y;
        int app = ov_list[t];
        bool sel = (t == ov_sel);
        fill_glass(x, y, tw, ov_th, 12, C_GLASS_TILE, sel ? GLASS_TILE + 25 : GLASS_TILE);
        draw_round_rect(x, y, tw, ov_th, 12, sel ? C_ACCENT : C_EDGE);
        int s = (tw >= 170) ? 2 : 1;                     // крупная плитка — крупная иконка
        int gs = 20 * s;
        draw_icon20s(x + (tw - gs) / 2, y + (ov_th - gs - 22) / 2,
                     dock_icons[app], s, C_WHITE);
        const char* nm = dock_names[app];
        int nn = 0; while (nm[nn]) nn++;
        draw_text(x + (tw - nn * 8) / 2, y + ov_th - 20, nm,
                  sel ? C_WHITE : C_TEXT_DIM);
    }
    if (ov_n == 0)                                       // пустой поиск — поймём
        draw_text((W - 10 * 8) / 2, gy + 24, "No matches", C_TEXT_DIM);

    // подсказка внизу экрана
    const char* hint = "Arrows - choose   Enter - open   Esc - close";
    int hn = 0; while (hint[hn]) hn++;
    draw_text((W - hn * 8) / 2, H - 26, hint, 0x6E7488);
    comp_full = false;
}

// Открыть/закрыть обзор. При входе клавиатура переносится на обзор
// (win_focus = -1 «рабочий стол»), прежний фокус запоминается и
// восстанавливается при выходе (игра продолжает жить после F2).
static int ov_prev_focus = -1;

void overview_open() {
    ov_prev_focus = win_focus;               // кому вернуть фокус
    overview_active = true;
    ov_sel = 0; ov_qlen = 0; ov_query[0] = 0;
    win_focus = -1;
    draw_overview();
}

void overview_close() {
    overview_active = false;
    if (ov_prev_focus >= 0 && app_is_open(ov_prev_focus))   // окно ещё живо?
        win_focus = ov_prev_focus;
    else
        win_focus = -1;
    draw_desktop();
}

// Клавиатура обзора: навигация, поиск, запуск.
void overview_handle_key(int sc) {
    if (sc == 0xE0) { e0_prefix = true; return; }        // стрелки идут с E0
    bool ext = e0_prefix; e0_prefix = false;
    if (sc & 0x80) return;                               // отпускания — мимо

    if (ext) {
        if      (sc == 0x48 && ov_sel >= ov_cols) ov_sel -= ov_cols;   // Up
        else if (sc == 0x50 && ov_sel + ov_cols < ov_n) ov_sel += ov_cols; // Down
        else if (sc == 0x4B && ov_sel > 0)        ov_sel--;            // Left
        else if (sc == 0x4D && ov_sel < ov_n - 1) ov_sel++;            // Right
        else return;
        draw_overview();
        return;
    }
    if (sc == 0x01) { overview_close(); return; }        // Esc
    if (sc == SC_F1 || sc == SC_F2) { overview_close(); return; }
    if (sc == SC_F3) { overview_close(); cmd_open(); return; }
    if (sc == 0x1C) {                                    // Enter — запуск
        if (ov_n > 0) {
            int app = ov_list[ov_sel];
            overview_close();
            dock_launch(app);
        }
        return;
    }
    if (sc == 0x0E) {                                    // Backspace — стереть
        if (ov_qlen > 0) {
            ov_query[--ov_qlen] = 0;
            draw_overview();
        }
        return;
    }
    char c = 0;
    if (sc < 0x3A) {                                     // обычный символ
        c = apply_shift(sc_ascii[sc]);                   // печатаем в поиск
    }
    if (c >= ' ' && c < 127 && ov_qlen < (int)sizeof(ov_query) - 1) {
        ov_query[ov_qlen++] = c;
        ov_query[ov_qlen] = 0;
        ov_sel = 0;                                      // фильтр сменился — с начала
        draw_overview();
    }
}

// Мышь обзора: наведение выбирает плитку (как в доке), щелчок по плитке
// запускает приложение, щелчок мимо (вне плиток и поиска) — на рабочий стол.
void overview_mouse(int press, int b) {
    // в строке поиска? тогда «мимо» — не закрываем обзор
    int sx = (W - ov_sw) / 2;
    bool in_search = (mouse_x >= sx && mouse_x < sx + ov_sw &&
                      mouse_y >= ov_sy && mouse_y < ov_sy + ov_sh);

    int hit = -1;
    for (int t = 0; t < ov_n; t++)
        if (mouse_x >= ov_tx[t] && mouse_x < ov_tx[t] + ov_tw &&
            mouse_y >= ov_ty[t] && mouse_y < ov_ty[t] + ov_th) { hit = t; break; }

    if (hit >= 0 && hit != ov_sel) { ov_sel = hit; draw_overview(); }
    if (!(press & 1)) return;                            // дальше — только левый клик
    if (hit >= 0) {
        int app = ov_list[ov_sel];
        overview_close();
        dock_launch(app);                                // (уже открытое — поднять)
        return;
    }
    if (!in_search) overview_close();
}
