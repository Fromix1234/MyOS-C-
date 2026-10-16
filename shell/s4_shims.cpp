// ===========================================================================
//  SHELL s4: «прослойка» — старые имена ядра через системные вызовы
//            + сбор ввода (клавиатура/мышь) из событий ядра
// ===========================================================================
//  Код окон и приложений (s5..s11) писался в v1.2 против функций ядра
//  (fs_ls, cmd_part, install_check, ...). В v2.0 те же функции живут в ядре,
//  но снаружи доступны только через сисколлы — этот файл даёт оболочке
//  привычные имена с той же сигнатурой. Никаких двойных сообщений: команды
//  ядра печатают в СИСТЕМНЫЙ ЛОГ, а консоль оболочки его показывает (dmesg).
// ===========================================================================

// --- Общие величины оболочки ------------------------------------------------
int demo_pid = -1;                     // задача demo.bin (команда ring3) или -1
unsigned int g_kernel_sectors = 0;     // размер ядра в секторах (вместо макроса)

// --- Время ------------------------------------------------------------------
unsigned int day_secs() { return (unsigned int)sys_time(); }
unsigned int millis()   { return (unsigned int)sys_ticks(); }

// --- Сбор ввода: пачкой забираем события ядра -------------------------------
int mouse_flag = 0;                    // mouse_read(): «для курсора есть новости»

void input_pump() {
    myos_event_t ev[32];
    for (int round = 0; round < 8; round++) {
        int n = sys_call2(SYS_INPUT, (int)ev, 32);
        if (n <= 0) break;
        for (int i = 0; i < n; i++) {
            if (ev[i].type == EV_KEY) {
                kbd_q_push(ev[i].d1);
            } else if (ev[i].type == EV_MOUSE) {
                if (ev[i].d1 || ev[i].d2) {              // движение
                    mouse_x += ev[i].d1;
                    mouse_y += ev[i].d2;
                    if (mouse_x < 0) mouse_x = 0;
                    if (mouse_x > W - 1) mouse_x = W - 1;
                    if (mouse_y < 0) mouse_y = 0;
                    if (mouse_y > H - 1) mouse_y = H - 1;
                    mouse_flag = 1;
                }
                if (ev[i].d3 != mouse_btn) {             // кнопки
                    mouse_btn = ev[i].d3;
                    mouse_flag = 1;
                }
            }
        }
    }
}
int mouse_read() { if (!mouse_flag) return 0; mouse_flag = 0; return 1; }

// --- Системный лог ядра (dmesg) — в окно консоли ----------------------------
// ВАЖНО: рисуем только когда окно консоли открыто. Иначе печать лога
// (например, boot-метки ядра) вылезала текстом прямо на рабочий стол.
void log_drain() {
    char b[128];
    for (int round = 0; round < 16; round++) {
        int n = sys_log_read(b, 128);
        if (n <= 0) break;
        if (cmd_active)                          // окно консоли открыто — печатаем
            for (int i = 0; i < n; i++) cmd_putchar(b[i]);
    }
}

// --- Файлы MyFS (имена функций и структура — как в v1.2) --------------------
struct FileInfo { char name[56]; unsigned int size; unsigned int start; };
FileInfo file_list[FS_ENT_MAX];

int fs_load_table(int n) {
    myos_file_t out[FS_ENT_MAX];
    int cnt = sys_call3(SYS_FS_LIST, n, (int)out, FS_ENT_MAX);
    if (cnt < 0) return cnt;                    // -1 нет раздела, -2 не MyFS
    for (int i = 0; i < cnt; i++) {
        int k = 0;
        while (k < 55 && out[i].name[k]) { file_list[i].name[k] = out[i].name[k]; k++; }
        file_list[i].name[k] = 0;
        file_list[i].size  = out[i].size;
        file_list[i].start = out[i].start;
    }
    return cnt;
}
void fs_ls(int n) { sys_call1(SYS_FS_LS, n); }                          // печать в лог
void fs_cat(int n, const char* name) { sys_call2(SYS_FS_CAT, n, (int)name); }
int fs_format(int n, bool quiet = false) {
    return sys_call2(SYS_FS_FORMAT, n, quiet ? 1 : 0);
}
int fs_write(int n, const char* name, const char* text, bool quiet = false) {
    return sys_call5(SYS_FS_WRITE, n, (int)name, (int)text, -1, quiet ? 1 : 0);
}
int fs_delete(int n, const char* name) { return sys_call2(SYS_FS_DELETE, n, (int)name); }
int fs_rename(int n, const char* oldname, const char* newname) {
    return sys_call3(SYS_FS_RENAME, n, (int)oldname, (int)newname);
}

// --- Просмотр файла (окно Files) — буфер, как в v1.2 ------------------------
const int VIEW_MAX = 2048;
char view_buf[VIEW_MAX];
unsigned int view_len = 0;

void fs_read_file(unsigned int start, unsigned int size) {
    view_len = 0;
    if (size > VIEW_MAX) size = VIEW_MAX;       // большие файлы — первые 2 КБ
    int got = sys_call4(SYS_FS_READAT, (int)start, (int)size, (int)view_buf, VIEW_MAX);
    if (got > 0) view_len = (unsigned int)got;
}
int fs_read_content(int idx, char* dst, int max) {
    return sys_call4(SYS_FS_READAT, (int)file_list[idx].start,
                     (int)file_list[idx].size, (int)dst, max);
}

// --- Разделы и установка (как v1.2) -----------------------------------------
void cmd_part() { sys_call0(SYS_PART_LS); }                             // печать в лог
void cmd_mkpart(int n, int mb) { sys_call2(SYS_PART_MK, n, mb); }
void cmd_wtest() { sys_call0(SYS_DISK_WTEST); }                         // печать в лог
void cmd_install(int n, bool yes) { sys_call2(SYS_INSTALL, n, yes ? 1 : 0); }

int boot_partition(unsigned short* out_lba) {
    myos_boot_t b;
    int idx = sys_call1(SYS_BOOTPART, (int)&b);
    if (out_lba) *out_lba = (unsigned short)b.lba;
    return idx;
}
bool install_check(int n, unsigned int* plba, unsigned int* psize) {
    myos_install_t plan;
    int r = sys_call2(SYS_INSTALL_PLAN, n, (int)&plan);
    if (r != 0) return false;                   // причина уже в системном логе
    if (plba)  *plba  = plan.lba;
    if (psize) *psize = plan.sectors;
    return true;
}
// Установка из графического мастера: ядру уходит flags = 3 (bit0 — подтверждено,
// ПИСАТЬ; bit1 — тихо, план уже показан в окне мастера). Раньше сюда уходил
// только флаг talk, и мастер отправлял 0 — ядро считало это «без yes» и НИЧЕГО
// не писало, а окно рисовало «Done!» (баг v2.0, найден тестом test_glass).
void install_do(int n, unsigned int plba, bool talk = true) {
    (void)plba;                                 // ядро само пересчитает план
    sys_call2(SYS_INSTALL, n, talk ? 1 : 3);
}
