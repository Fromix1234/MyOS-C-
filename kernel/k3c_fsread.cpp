// ===========================================================================
//  KERNEL k3c: MyFS file table + reading files into memory
// ===========================================================================
//  MyOS v2.0 module. Split mechanically from kernel.cpp v1.2
//  (git a81d4f4) and then reworked: see README section 11.


// Список файлов для проводника: копия таблицы MyFS выбранного раздела
struct FileInfo { char name[56]; unsigned int size; unsigned int start; };
FileInfo file_list[FS_FILES_MAX];

// Загрузить таблицу файлов раздела n в file_list.
// Возврат: сколько файлов (>0), 0 — пусто, -1 — нет раздела, -2 — не MyFS.
int fs_load_table(int n) {
    unsigned int plba, psize;
    if (!find_part(n, &plba, &psize)) return -1;
    ata_read_sector(plba + PART_RESERVED, disk_buf);
    if (!is_myfs()) return -2;
    Superblock* sb = (Superblock*)disk_buf;
    ata_read_sector(sb->table_lba, sec_buf);
    int cnt = 0;
    for (int i = 0; i < FS_FILES_MAX; i++) {
        FileEntry* f = (FileEntry*)(sec_buf + i * 64);
        if (f->name[0] == 0) continue;               // пустая запись
        for (int k = 0; k < 56; k++) file_list[cnt].name[k] = f->name[k];
        file_list[cnt].size  = f->size;
        file_list[cnt].start = f->start;
        cnt++;
    }
    return cnt;
}

// --- Чтение файлов в буфер (для SYS_FS_READ/SYS_FS_READAT и загрузки ядром) --

// Прочитать size байт с LBA start в буфер dst (не больше max).
// Возврат: сколько байт прочитано. Буфер ЯДРА (внутренние вызовы).
int fs_read_at(unsigned int start, unsigned int size, unsigned char* dst, unsigned int max) {
    if (size > max) size = max;
    unsigned int got = 0;
    while (got < size) {
        ata_read_sector(start, disk_buf);
        unsigned int chunk = size - got;
        if (chunk > 512) chunk = 512;
        for (unsigned int i = 0; i < chunk; i++) dst[got + i] = disk_buf[i];
        got += chunk;
        start++;
    }
    return (int)got;
}

// Прочитать файл по имени из раздела n в буфер dst. Возврат: байт или -1.
// Так ядро грузит оболочку (shell.bin) и демо-программу (demo.bin).
int fs_read_by_name(int n, const char* name, unsigned char* dst, unsigned int max) {
    int cnt = fs_load_table(n);
    if (cnt < 0) return -1;
    for (int i = 0; i < cnt; i++) {
        const char* a = file_list[i].name;
        const char* b = name;
        while (*a && *a == *b) { a++; b++; }
        if (*a != *b) continue;                      // имена разошлись
        return fs_read_at(file_list[i].start, file_list[i].size, dst, max);
    }
    return -1;                                       // файла нет
}
