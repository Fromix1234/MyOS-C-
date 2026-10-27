// ===========================================================================
//  SHELL s11: input dispatch + desktop compositor + mouse
// ===========================================================================
//  MyOS v2.0 module. Split mechanically from kernel.cpp v1.2
//  (git a81d4f4) and then reworked: see README section 11.

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
    if (sc == SC_F2) { overview_open(); return; }    // F2 — экран Activities
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

// Нарисовать одно окно (каждое рисует себя по своим win_x/win_y).
// Перед отрисовкой сообщаем draw_window_frame, в фокусе ли окно: рамка и
// «светофор» активного окна ярче, у остальных — тусклее.
void draw_app(int a) {
    g_frame_focus = (win_focus == a);
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
    draw_branding();                         // фирменная карточка под окнами
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
    if (i < 0 || i >= DOCK_N) return;            // ничего не выбрано (курсор ушёл с дока)
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
        if (ny < MB_H + 4) ny = MB_H + 4;
        if (ny + win_hp[drag] > H - 104) ny = H - 104 - win_hp[drag];
        if (ny < MB_H + 4) ny = MB_H + 4;
        if (nx != win_x[drag] || ny != win_y[drag]) {
            win_x[drag] = nx; win_y[drag] = ny;
            draw_desktop();
        }
        return;
    }

    // --- экран Activities перехватывает мышь целиком ---
    if (overview_active) { overview_mouse(press, b); return; }

    // --- подсветка иконки дока под курсором ---
    // Перерисовываем ТОЛЬКО док: полный кадр на каждое движение мыши заставлял
    // экран мигать и тормозить (окна до дока всё равно не достают).
    int hov = dock_hit(mouse_x, mouse_y);
    if (hov >= 0 && hov != dock_sel) { dock_sel = hov; draw_dock_area(); }
    else if (hov < 0 && dock_sel >= 0) { dock_sel = -1; draw_dock_area(); }  // курсор ушёл — подсветку/подпись снять

    if (!(press & 1)) return;                  // дальше — только щелчок левой кнопкой

    // Один полный кадр на клик: его нарисует сам запуск (открытие или подъём окна)
    if (hov >= 0) { dock_sel = hov; dock_launch(hov); return; }

    int a = win_hit(mouse_x, mouse_y);
    if (a < 0) return;
    int dx = mouse_x - win_x[a], dy = mouse_y - win_y[a];

    // «светофор» в шапке: красная кнопка (x+14, центр шапки) — закрыть окно
    if (dy < TITLE_H && (dx - 14) * (dx - 14) + (dy - TITLE_H / 2) * (dy - TITLE_H / 2) <= 49) {
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

