// ===========================================================================
//  SHELL s3: mouse state, cursor, console (CMD), string utils
// ===========================================================================
//  MyOS v2.0 module. Split mechanically from kernel.cpp v1.2
//  (git a81d4f4) and then reworked: see README section 11.

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
