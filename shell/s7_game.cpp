// ===========================================================================
//  SHELL s7: game (Snake)
// ===========================================================================
//  MyOS v2.0 module. Split mechanically from kernel.cpp v1.2
//  (git a81d4f4) and then reworked: see README section 11.

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

// Поле игры (счёт, рамка поля, еда и змейка) — без рамки окна.
// Используется и полным кадром (draw_game), и быстрым тик-перерисовщиком:
// игра шагает каждые ~140 мс, и полный кадр (обои пер-пиксельно + все окна)
// на каждый шаг — это секунды простоя на перерисовку. Окно не двигается,
// поэтому достаточно перерисовать его внутренность.
static void draw_game_field() {
    int tx = gwx + 10, ty = gwy + TITLE_H + 6;
    char sc[48]; int k = sk_str(sc, 0, "Score: "); k = sk_num(sc, k, (unsigned int)gscore);
    k = sk_str(sc, k, "   Length: "); k = sk_num(sc, k, (unsigned int)gslen);
    sc[k] = 0;
    // строка счёта очищается «стеклом тела окна» (alpha 256 = плотно):
    fill_glass(gwx + 8, ty - 2, gww - 16, 16, 0, C_GLASS_BODY, 256);
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
        fill_glass(gwx + 8, gwy + gwh - 22, gww - 16, 16, 0, C_GLASS_BODY, 256);
        draw_text(tx, gwy + gwh - 20, "Arrows - move   Esc - exit", TEXT_COL);
    }
}

// Нарисовать окно игры целиком
void draw_game() {
    game_layout();
    draw_window_frame(gwx, gwy, gww, gwh, "Snake");
    draw_game_field();
}

// Перерисовать только внутренность окна игры (поле, счёт, подсказка):
// на каждый такт полный кадр не нужен — обои уже под окном не меняются.
void game_redraw() { cursor_hide(); draw_game_field(); cursor_show(); }

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
