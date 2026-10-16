// ===========================================================================
//  KERNEL k4c: boot_partition lookup
// ===========================================================================
//  MyOS v2.0 module. Split mechanically from kernel.cpp v1.2
//  (git a81d4f4) and then reworked: see README section 11.

// Из какого раздела загрузились? VBR оставил «записку» в низкой памяти:
// 0x0500 = LBA раздела (word), 0x0502 = сигнатура 'MY' (0x4D59).
// Возврат: 1..4 — номер раздела, 0 — записка есть, но раздела нет, -1 — не через VBR.
int boot_partition(unsigned short* out_lba) {
    if (*(volatile unsigned short*)0x0502 != 0x4D59) return -1;
    unsigned short bootlba = *(volatile unsigned short*)0x0500;
    if (out_lba) *out_lba = bootlba;
    ata_read_sector(0, disk_buf);        // читаем MBR — ищем раздел с таким LBA
    for (int i = 0; i < 4; i++) {
        unsigned char* e = disk_buf + 446 + i * 16;
        if (e[4] == 0) continue;         // пустой слот
        unsigned int l = (unsigned int)e[8] | ((unsigned int)e[9] << 8) |
                         ((unsigned int)e[10] << 16) | ((unsigned int)e[11] << 24);
        if (l == bootlba) return i + 1;  // нашли — это наш раздел
    }
    return 0;
}

// --- Файлы системы и снимки в ОЗУ ------------------------------------------
// Раздел, с которого загружены файлы системы (shell.bin/demo.bin).
int sys_part = 0;
// Снимки файлов системы: 1 МБ под оболочку (адрес 0x100000) и 1 МБ под демо
// (0x200000). Из них SYS_SPAWN берёт программу, а SYS_FS_FORMAT/SYS_INSTALL
// возвращают файлы на место — сами в образ ядра они не «вшиты».
unsigned char* shell_snap = (unsigned char*)0x100000;
unsigned int   shell_snap_len = 0;
unsigned char* demo_snap  = (unsigned char*)0x200000;
unsigned int   demo_snap_len  = 0;

// Есть ли на разделе n файл с именем name? 1 — есть, 0 — нет.
int part_has_file(int n, const char* name) {
    int cnt = fs_load_table(n);
    if (cnt <= 0) return 0;
    for (int i = 0; i < cnt; i++) {
        const char* a = file_list[i].name;
        const char* b = name;
        while (*a && *a == *b) { a++; b++; }
        if (*a == *b) return 1;
    }
    return 0;
}

// Найти раздел, на котором есть файл name (перебор P1..P4). 0 — не нашли.
// Нужно, когда загрузочный раздел — MyFS без файлов системы (после install):
// оболочка честно догрузится с другого раздела.
int find_part_with_file(const char* name) {
    for (int n = 1; n <= 4; n++)
        if (part_has_file(n, name)) return n;
    return 0;
}
