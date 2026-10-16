// ===========================================================================
//  SHELL s6: apps - Files, Settings, Task Manager
// ===========================================================================
//  MyOS v2.0 module. Split mechanically from kernel.cpp v1.2
//  (git a81d4f4) and then reworked: see README section 11.

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
            k = sk_num(b, k, (unsigned int)(g_kernel_sectors * 512 / 1024));
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

// Текущее время в секундах суток — функция day_secs() теперь в прослойке
// (s4_shims): спрашивает ядро системным вызовом SYS_TIME.

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
    k = sk_num(b, k, (unsigned int)(g_kernel_sectors * 512 / 1024));
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
    tasks_last_sec = (int)(day_secs() % 60);
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
    int s = (int)(day_secs() % 60);
    if (s == tasks_last_sec) return;             // та же секунда — не трогаем экран
    tasks_last_sec = s;
    cursor_hide();
    tasks_draw_uptime();
    cursor_show();
}
