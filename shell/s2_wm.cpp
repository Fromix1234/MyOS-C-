// ===========================================================================
//  SHELL s2: prototypes, window manager, keyboard table
// ===========================================================================
//  MyOS v2.0 module. Split mechanically from kernel.cpp v1.2
//  (git a81d4f4) and then reworked: see README section 11.

// ===========================================================================
//  ПРОТОТИПЫ (объявления функций, вызываемых раньше их определения)
// ===========================================================================
void run_command(const char* line);
void draw_desktop();
void draw_menubar();
int  boot_partition(unsigned short* out_lba);
void game_open();                        // мини-игра «Змейка»
void code_open();                        // редактор кода MyC
bool app_is_open(int a);                 // открыто ли окно приложения (s11)
void fill_wallpaper();                   // обои целиком (s1)
unsigned int wallpaper_pixel(int x, int y);   // цвет обоев в точке (s0)
unsigned int mix_col(unsigned int c1, unsigned int c2, int a);  // в s0

// Обзор Activities (s13): полноэкранная сетка приложений с поиском (F2)
bool overview_active = false;
void overview_open();
void overview_close();
void draw_overview();
void overview_handle_key(int sc);
void overview_mouse(int press, int b);

void win_place(int a, int w, int h);     // оконный менеджер: поставить окно
void win_raise(int a);                   // поднять окно наверх (и дать фокус)
void dock_launch(int i);                 // запуск/подъём приложения из дока
void inst_open();                        // графический мастер установки (setup)
int  win_w(int want);                    // размер окна — не больше экрана
int  win_h(int want);

// ===========================================================================
//  ОКОННЫЙ МЕНЕДЖЕР — несколько окон на экране одновременно
// ===========================================================================
//  У каждого приложения есть окно со своим положением и «слоем» (z-порядком).
//  Номера приложений совпадают с порядком иконок в доке.
// APP_INSTALL — графический мастер установки: иконки в доке у него нет
// (он вызывается командой setup в консоли), но живёт он по тем же законам:
// своё окно, свой слой, свой фокус клавиатуры.
enum { APP_CMD = 0, APP_FILES, APP_SET, APP_TASKS, APP_CODE, APP_GAME, APP_ABOUT, APP_INSTALL, APP_N };

int  win_x[APP_N], win_y[APP_N];       // положение окна на экране
int  win_wp[APP_N], win_hp[APP_N];     // размер окна (нужен для попадания мышью)
int  win_z[APP_N];                     // слой: больше — выше
int  win_ztop = 0;                     // верхний занятый слой
int  win_focus = -1;                   // у какого окна клавиатура (-1 — рабочий стол)
int  win_casc = 0;                     // счётчик «лестницы» для новых окон

// Поднять окно наверх и отдать ему клавиатуру
void win_raise(int a) {
    if (a < 0 || a >= APP_N) return;
    win_z[a] = ++win_ztop;
    win_focus = a;
}

// Поставить новое окно «лестницей» от центра, не вылезая за экран
void win_place(int a, int w, int h) {
    win_wp[a] = w; win_hp[a] = h;
    int st = win_casc % 6; win_casc++;
    int x = (W - w) / 2 - 110 + st * 46;
    int y = 30 + st * 38;
    int top = MB_H + 6, bot = 104;       // меню-бар+отступ / место под док
    if (x < 4) x = 4;
    if (x + w > W - 4) x = W - 4 - w;
    if (x < 4) x = 4;
    if (y < top) y = top;
    if (y + h > H - bot) y = H - bot - h;
    if (y < top) y = top;
    win_x[a] = x; win_y[a] = y;
}

// Закрыть окно: фокус уходит верхнему из оставшихся (win_z>0 — «открыто»)
void win_close_app(int a) {
    if (a < 0 || a >= APP_N) return;
    win_z[a] = 0;
    int best = -1, bz = 0;
    for (int i = 0; i < APP_N; i++)
        if (i != a && win_z[i] > bz) { bz = win_z[i]; best = i; }
    win_focus = best;
}

// Положение консоли: её внутренние координаты TXT_* зависят от WIN_*
void console_set_pos(int x, int y) {
    WIN_X = x; WIN_Y = y;
    TXT_X = WIN_X + 6;
    TXT_Y = WIN_Y + TITLE_H + 6;
}

// ===========================================================================
//  КЛАВИАТУРА (опрос портов 0x64/0x60, набор сканкодов Set 1)
// ===========================================================================
//  Свой обработчик прерываний (IDT/IRQ) пока не делаем — вместо этого просто
//  ОПРАШИВАЕМ контроллер клавиатуры: порт 0x64 бит0 = «есть данные»,
//  порт 0x60 = сам сканкод. Работает и без прерываний.
const int SC_F3 = 0x3D;                  // сканкод F3 (нажатие)
const int SC_F2 = 0x3C;                  // сканкод F2 (экран Activities)
const int SC_F1 = 0x3B;                  // сканкод F1 (смена фокуса окон)

// Таблица: сканкод -> символ (строчные/цифры/знаки). 0 = спецклавиша.
unsigned char sc_ascii[0x3A] = {
    // 0x00..0x0F
    0, 0,'1','2','3','4','5','6','7','8','9','0','-', '=', 0, 0,
    // 0x10..0x1F
   'q','w','e','r','t','y','u','i','o','p','[',']', 0, 0,'a','s',
    // 0x20..0x2F
   'd','f','g','h','j','k','l',';','\'','`', 0,'\\','z','x','c','v',
    // 0x30..0x39
   'b','n','m',',','.','/', 0,'*', 0,' '
};

bool shift_down = false;                 // зажат ли Shift

// Применить Shift: буква -> ЗАГЛАВНАЯ, цифра/знак -> спецсимвол
char apply_shift(char c) {
    if (c >= 'a' && c <= 'z') return c - 32;
    switch (c) {
        case '1': return '!'; case '2': return '@'; case '3': return '#';
        case '4': return '$'; case '5': return '%'; case '6': return '^';
        case '7': return '&'; case '8': return '*'; case '9': return '(';
        case '0': return ')'; case '-': return '_'; case '=': return '+';
        case '[': return '{'; case ']': return '}'; case ';': return ':';
        case '\'': return '"'; case '`': return '~'; case '\\': return '|';
        case ',': return '<'; case '.': return '>'; case '/': return '?';
    }
    return c;
}
