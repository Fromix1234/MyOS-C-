// ===========================================================================
//  KERNEL k3: MyFS on-disk structures + partition lookup
// ===========================================================================
//  MyOS v2.0 module. Split mechanically from kernel.cpp v1.2
//  (git a81d4f4) and then reworked: see README section 11.

// ===========================================================================
//  MyFS — СВОЯ ФАЙЛОВАЯ СИСТЕМА (суперблок + таблица файлов + данные)
// ===========================================================================
//  Раскладка раздела:
//    +0 : суперблок      (magic "MYFS", размеры, где таблица, где данные)
//    +1 : таблица файлов (8 записей по 64 байта = ровно 1 сектор)
//    +2 : область данных (файлы лежат подряд, один за другим)
//  Файлы хранятся ЦЕЛИКОМ подряд — для учебной ОС этого достаточно, а устроено
//  всё как у взрослых ФС: метаданные (суперблок, таблица) отдельно от данных.

struct Superblock {
    char magic[4];                 // 'M','Y','F','S'
    unsigned short version;        // версия формата = 1
    unsigned int total_sectors;    // размер раздела в секторах
    unsigned int table_lba;        // LBA таблицы файлов
    unsigned int data_lba;         // LBA начала области данных
    unsigned int data_ptr;         // первый СВОБОДНЫЙ сектор данных
    unsigned short file_count;     // сколько записей занято
    unsigned short files_max;      // сколько записей всего (8)
} __attribute__((packed));

struct FileEntry {
    char name[56];                 // имя файла (заканчивается нулём)
    unsigned int start;            // LBA начала данных
    unsigned int size;             // размер в байтах
} __attribute__((packed));

const int FS_FILES_MAX = 8;        // 8 записей по 64 байта = ровно 1 сектор
unsigned char sec_buf[512];        // второй буфер для ФС (первый занят MBR)

// Первые PART_RESERVED секторов ЛЮБОГО раздела — служебная зона загрузки
// (её пишет install):
//   +0     VBR — загрузочный сектор РАЗДЕЛА (копия boot.asm с метками раздела)
//   +1..+2 stage2 (2 сектора: VESA + загрузка ядра)
//   +3..   ядро (сколько секторов — знает константа KERNEL_SECTORS)
// MyFS начинается ТОЛЬКО после этой зоны, поэтому format и install не мешают
// друг другу: install перезаписывает зону загрузки, файлы MyFS остаются целы.
// Значение PART_RESERVED теперь одно на всю систему — в common/myos.h.

// Найти раздел N в MBR: вернуть LBA начала и размер (в секторах).
// false = раздела нет. ВНИМАНИЕ: читает сектор 0 в disk_buf и затирает его!
bool find_part(int n, unsigned int* lba, unsigned int* size) {
    if (n < 1 || n > 4) return false;                 // защита от выхода за буфер
    ata_read_sector(0, disk_buf);
    if (disk_buf[510] != 0x55 || disk_buf[511] != 0xAA) return false;
    unsigned char* e = disk_buf + 446 + (n - 1) * 16;
    if (e[4] == 0) return false;                      // слот пуст
    *lba  = (unsigned int)e[8]  | ((unsigned int)e[9]  << 8) |
            ((unsigned int)e[10] << 16) | ((unsigned int)e[11] << 24);
    *size = (unsigned int)e[12] | ((unsigned int)e[13] << 8) |
            ((unsigned int)e[14] << 16) | ((unsigned int)e[15] << 24);
    return true;
}

// В disk_buf сейчас суперблок MyFS?
bool is_myfs() {
    return disk_buf[0] == 'M' && disk_buf[1] == 'Y' &&
           disk_buf[2] == 'F' && disk_buf[3] == 'S';
}
int sk_str(char* b, int k, const char* s) { while (*s) b[k++] = *s++; return k; }
int sk_num(char* b, int k, unsigned int v) {
    char t[12]; int i = 0;
    if (v == 0) t[i++] = '0';
    while (v > 0) { t[i++] = (char)('0' + v % 10); v /= 10; }
    while (i > 0) b[k++] = t[--i];
    return k;
}
int sk_hex8(char* b, int k, unsigned char v) {
    const char* h = "0123456789ABCDEF";
    b[k++] = h[(v >> 4) & 0x0F];
    b[k++] = h[v & 0x0F];
    return k;
}
// 4 байта (32-битное число) в hex — для адресов ОЗУ
int sk_hex32(char* b, int k, unsigned int v) {
    k = sk_hex8(b, k, (unsigned char)((v >> 24) & 0xFF));
    k = sk_hex8(b, k, (unsigned char)((v >> 16) & 0xFF));
    k = sk_hex8(b, k, (unsigned char)((v >> 8) & 0xFF));
    return sk_hex8(b, k, (unsigned char)(v & 0xFF));
}

// --- Функции сравнения строк (нужны и ФС, и командам ядра) -----------------
bool str_eq(const char* a, const char* b) {
    while (*a && *b) { if (*a != *b) return false; a++; b++; }
    return *a == *b;
}
bool str_startswith(const char* s, const char* p) {
    while (*p) { if (*s != *p) return false; s++; p++; }
    return true;
}
