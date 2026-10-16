// ===========================================================================
//  SHELL s9: installer wizard UI (incl. inst_scan)
// ===========================================================================
//  MyOS v2.0 module. Split mechanically from kernel.cpp v1.2
//  (git a81d4f4) and then reworked: see README section 11.

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
    myos_part_t p[4];                        // v2.0: таблицу разделов читает ЯДРО
    sys_call2(SYS_PART_LIST, (int)p, 4);     // флаги: 1 занят, 2 MyFS, 4 годен, 8 актив
    for (int i = 0; i < 4; i++) {
        if (!p[i].lba) continue;             // пустой слот
        inst_lba[i] = p[i].lba;
        inst_mb[i]  = p[i].sectors / 2048;
        inst_fl[i]  = (int)p[i].flags;
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
        k = sk_num(b, k, g_kernel_sectors);
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
