// ===========================================================================
//  myos.h — ОБЩИЙ ДОГОВОР МЕЖДУ ЯДРОМ И ОБОЛОЧКОЙ (как в Linux)
// ===========================================================================
//  Этот файл подключают И ядро (ring 0), И графическая оболочка (ring 3).
//  Здесь описан только «стык»: номера системных вызовов, структуры данных
//  (видеопамять, разделы, файлы, события ввода) и константы раскладки.
//
//  Правило Zen/Linux: ядро и оболочка — РАЗНЫЕ сборки, общаются ТОЛЬКО
//  через системные вызовы (int 0x80: eax=номер, ebx/ecx/edx/esi/edi=аргументы,
//  ответ в eax). Никаких общих переменных между ними нет — только этот файл.
// ===========================================================================
#ifndef MYOS_H
#define MYOS_H

#define MYOS_VERSION "MyOS v2.0"

// --- Раскладка адресного пространства программ (ring 3) --------------------
//  Ядро отдаёт программам 4..8 МБ (страницы по 4 МБ):
//    4..5 МБ — ОБОЛОЧКА (shell.bin): тут живёт весь рабочий стол и окна;
//    5..6 МБ — ДЕМО-ПРОГРАММА (demo.bin, команда ring3);
//    6..7 МБ — зарезервировано под будущие программы.
#define SHELL_BASE      0x00400000u
#define SHELL_STACK_TOP 0x004FFFF0u
#define DEMO_BASE       0x00500000u
#define DEMO_STACK_TOP  0x005FFFF0u
#define USER_TOP        0x00800000u

// --- Раскладка раздела (MyFS) ----------------------------------------------
//  Первые PART_RESERVED секторов раздела — зона загрузки (VBR, stage2, ядро).
//  MyFS начинается после неё. Значение обязано совпадать с ядром.
#define PART_RESERVED   192u
#define FS_ENT_MAX      8

// --- Системные вызовы ------------------------------------------------------
#define SYS_EXIT        0    // (code) — завершить программу
#define SYS_KEY         1    // () -> сканкод из очереди владельца ввода или -1
#define SYS_TIME        2    // () -> секунды суток из часов CMOS
#define SYS_TICKS       3    // () -> миллисекунды с запуска (тики таймера)
#define SYS_PID         4    // () -> мой номер задачи
#define SYS_FB          5    // (myos_fb_t* out) — где видеопамять (/dev/fb0)
#define SYS_PRINT       6    // (const char*) — строка в системный лог (dmesg)
#define SYS_LOG_READ    7    // (char* dst, int max) -> прочитать системный лог
#define SYS_FS_LIST     8    // (int n, myos_file_t* out, int max) -> файлов
#define SYS_FS_READ     9    // (int n, const char* name, void* dst, int max)
#define SYS_FS_READAT  10    // (start_lba, size, void* dst, int max) -> байт
#define SYS_FS_WRITE   11    // (n, name, data, len, quiet): len=-1 — строка до нуля
#define SYS_FS_DELETE  12    // (int n, const char* name)
#define SYS_FS_RENAME  13    // (int n, const char* old, const char* new)
#define SYS_FS_FORMAT  14    // (int n) — формат MyFS (файлы системы сохраняются)
#define SYS_PART_LIST  15    // (myos_part_t out[4]) -> занятых разделов
#define SYS_PART_MK    16    // (int n, int mb, myos_mkpart_t* out)
#define SYS_DISK_WTEST 17    // () -> -1 ок, иначе номер плохого байта
#define SYS_INSTALL_PLAN 18  // (int n, myos_install_t* out) -> код
#define SYS_INSTALL    19    // (int n) -> код (пишет зону загрузки + файлы системы)
#define SYS_BOOTPART   20    // (myos_boot_t* out) — из какого раздела загрузились
#define SYS_SPAWN      21    // (const char* name, unsigned int base) -> pid | -1
#define SYS_ALIVE      22    // (pid) -> 1/0
#define SYS_INFO       23    // (what) -> число (0=KERNEL_SECTORS, 1=PART_RESERVED)
#define SYS_INPUT      24    // (myos_event_t* out, int max) -> сколько событий
#define SYS_FS_LS      25    // (int n) — список файлов раздела в системный лог
#define SYS_FS_CAT     26    // (int n, const char* name) — файл в системный лог
#define SYS_PART_LS    27    // () — таблица разделов в системный лог (как 'part')
#define SYS_YIELD      28    // () — уступить процессор другим задачам

// --- Коды ошибок (системные вызовы) ----------------------------------------
#define SYS_ERR_OK      0
#define SYS_ERR_PART   -1    // нет такого раздела
#define SYS_ERR_MYFS   -2    // раздел не отформатирован (не MyFS)
#define SYS_ERR_EXIST  -3    // имя занято / файл не найден (по контексту)
#define SYS_ERR_FULL   -4    // таблица файлов заполнена
#define SYS_ERR_SPACE  -5    // нет места
#define SYS_ERR_SYSFIL -9    // системный файл (shell.bin/demo.bin) не трогать

// --- Структуры данных «стыка» ----------------------------------------------
// Где видеопамять (заполняет ядро по SYS_FB). Как /dev/fb0 в Linux:
// оболочка рисует пиксели напрямую, а на кадры сисколлы не нужны.
struct myos_fb_t {
    unsigned int addr;      // адрес видеопамяти (identity-отображён и в ring 3)
    int w, h, bpp, pitch;   // ширина, высота, бит/пиксель, байт в строке
    int rpos, gpos, bpos;   // позиции бит R/G/B (порядок каналов от VESA)
};

// Описание раздела MBR (SYS_PART_LIST)
struct myos_part_t {
    unsigned int lba;       // начало (сектор)
    unsigned int sectors;   // размер (сектор)
    unsigned int flags;     // биты: 1 занят, 2 MyFS, 4 можно ставить, 8 активный
};

// Результат mkpart (SYS_PART_MK): куда встал раздел и сколько было свободно
struct myos_mkpart_t {
    unsigned int start;     // LBA начала созданного раздела
    unsigned int free_mb;   // сколько МБ оставалось (для сообщения об ошибке)
    int code;               // 0 ок, -1 плохой номер, -2 плохой размер,
                            // -3 нет MBR, -4 не хватает места
};

// План установки (SYS_INSTALL_PLAN)
struct myos_install_t {
    unsigned int lba;         // LBA раздела
    unsigned int sectors;     // размер раздела
    unsigned int kern_sectors; // размер ядра в секторах
    int code;                 // 0 ок, -1 нет раздела, -2 мал, -3 ядро не влезло
};

// Из какого раздела загрузились (SYS_BOOTPART)
struct myos_boot_t {
    int index;              // 1..4, 0 — в таблице не найден, -1 — raw-загрузка
    unsigned int lba;       // LBA раздела (из «записки» VBR)
};

// Файл MyFS (SYS_FS_LIST). Имена до 55 символов + ноль.
struct myos_file_t {
    char name[56];
    unsigned int start;     // LBA начала данных
    unsigned int size;      // размер в байтах
};

// Событие ввода (SYS_INPUT): оболочка забирает клавиатуру и мышь ПАЧКОЙ.
// Если вызывающий — владелец ввода (обычно сама оболочка), приходят и клавиши;
// иначе только мышь (клавиши остаются программе-владельцу — см. SYS_SPAWN).
#define EV_KEY    1         // d1 = сканкод (make/break, как есть)
#define EV_MOUSE  2         // d1 = dx, d2 = dy (экранные, вниз+), d3 = кнопки
struct myos_event_t {
    int type;               // EV_KEY или EV_MOUSE
    int d1, d2, d3;         // данные события (см. выше)
};

#endif // MYOS_H
