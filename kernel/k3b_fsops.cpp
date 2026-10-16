// ===========================================================================
//  KERNEL k3b: MyFS file operations (format/list/write/delete/rename/cat)
// ===========================================================================
//  MyOS v2.0 module. Split mechanically from kernel.cpp v1.2
//  (git a81d4f4) and then reworked: see README section 11.

int fs_format(int n, bool quiet = false) {
    unsigned int plba, psize;
    if (!find_part(n, &plba, &psize)) {
        if (!quiet) cmd_print("No such partition\n");
        return -1;
    }
    for (int i = 0; i < 512; i++) disk_buf[i] = 0;    // чистим сектор суперблока
    Superblock* sb = (Superblock*)disk_buf;
    sb->magic[0] = 'M'; sb->magic[1] = 'Y'; sb->magic[2] = 'F'; sb->magic[3] = 'S';
    sb->version       = 1;
    sb->total_sectors = psize;
    sb->table_lba     = plba + PART_RESERVED + 1;     // таблица — сразу за суперблоком
    sb->data_lba      = plba + PART_RESERVED + 2;     // а дальше — данные
    sb->data_ptr      = plba + PART_RESERVED + 2;     // данных пока нет
    sb->file_count    = 0;
    sb->files_max     = FS_FILES_MAX;
    ata_write_sector(plba + PART_RESERVED, disk_buf);
    for (int i = 0; i < 512; i++) disk_buf[i] = 0;    // пустая таблица файлов
    ata_write_sector(plba + PART_RESERVED + 1, disk_buf);
    if (!quiet) {
        cmd_print("Formatted P");
        cmd_putchar((char)('0' + n));
        cmd_print(" as MyFS\n");
    }
    return 0;
}

// Список файлов раздела N
void fs_ls(int n) {
    unsigned int plba, psize;
    if (!find_part(n, &plba, &psize)) { cmd_print("No such partition\n"); return; }
    ata_read_sector(plba + PART_RESERVED, disk_buf);
    if (!is_myfs()) { cmd_print("Not MyFS (format it first)\n"); return; }
    Superblock* sb = (Superblock*)disk_buf;
    ata_read_sector(sb->table_lba, sec_buf);
    cmd_print("MyFS on P");
    cmd_putchar((char)('0' + n));
    cmd_print(":\n");
    int shown = 0;
    for (int i = 0; i < FS_FILES_MAX; i++) {
        FileEntry* f = (FileEntry*)(sec_buf + i * 64);
        if (f->name[0] == 0) continue;                // пустая запись
        cmd_print("  ");
        cmd_print(f->name);
        cmd_print("  ");
        print_dec(f->size);
        cmd_print(" bytes\n");
        shown++;
    }
    if (shown == 0) cmd_print("  (empty)\n");
}

// Записать файл name с текстом text в раздел N.
// Возврат: 0 — ок, -1 — нет раздела, -2 — не MyFS, -3 — имя занято,
//          -4 — таблица заполнена, -5 — нет места.
// quiet — без печати в консоль (так пишет проводник).
int fs_write(int n, const char* name, const char* text, bool quiet = false) {
    unsigned int plba, psize;
    if (!find_part(n, &plba, &psize)) {
        if (!quiet) cmd_print("No such partition\n");
        return -1;
    }
    ata_read_sector(plba + PART_RESERVED, disk_buf);
    if (!is_myfs()) {
        if (!quiet) cmd_print("Not MyFS (format it first)\n");
        return -2;
    }
    Superblock* sb = (Superblock*)disk_buf;
    unsigned int table = sb->table_lba;
    unsigned int dptr  = sb->data_ptr;
    // читаем таблицу: ищем свободную запись и проверяем дубликаты имён
    ata_read_sector(table, sec_buf);
    int free_idx = -1;
    for (int i = 0; i < FS_FILES_MAX; i++) {
        FileEntry* f = (FileEntry*)(sec_buf + i * 64);
        if (f->name[0] == 0) { if (free_idx < 0) free_idx = i; continue; }
        if (str_eq(f->name, name)) {
            if (!quiet) cmd_print("File exists\n");
            return -3;
        }
    }
    if (free_idx < 0) {
        if (!quiet) cmd_print("File table full\n");
        return -4;
    }
    int len = 0;
    while (text[len]) len++;                          // длина текста
    unsigned int need = ((unsigned int)len + 511) / 512;   // сколько секторов занять
    if (dptr + need > plba + psize) {
        if (!quiet) cmd_print("No space on partition\n");
        return -5;
    }
    // обновляем суперблок СРАЗУ, пока disk_buf ещё держит его в руках
    sb->data_ptr = dptr + need;
    sb->file_count = (unsigned short)(sb->file_count + 1);
    ata_write_sector(plba + PART_RESERVED, disk_buf);
    // пишем данные: каждые 512 байт текста -> один сектор
    for (unsigned int s = 0; s < need; s++) {
        for (int i = 0; i < 512; i++) {
            int pos = (int)(s * 512) + i;
            disk_buf[i] = (pos < len) ? (unsigned char)text[pos] : 0;
        }
        ata_write_sector(dptr + s, disk_buf);
    }
    // заполняем запись в таблице и пишем её на диск
    FileEntry* f = (FileEntry*)(sec_buf + free_idx * 64);
    for (int i = 0; i < 56; i++) f->name[i] = 0;
    int k = 0;
    while (name[k] && k < 55) { f->name[k] = name[k]; k++; }
    f->start = dptr;
    f->size  = (unsigned int)len;
    ata_write_sector(table, sec_buf);
    if (!quiet) {
        cmd_print("Written "); cmd_print(name);
        cmd_print(", "); print_dec((unsigned int)len); cmd_print(" bytes\n");
    }
    return 0;
}

// Удалить файл name из раздела N. Данные «уплотняются»: файлы, лежавшие
// после удалённого, сдвигаются на его место — освободившееся место можно
// сразу занять новыми файлами.
// Возврат: 0 — ок, -1 — нет раздела, -2 — не MyFS, -3 — нет файла.
int fs_delete(int n, const char* name) {
    unsigned int plba, psize;
    if (!find_part(n, &plba, &psize)) return -1;
    ata_read_sector(plba + PART_RESERVED, disk_buf);
    if (!is_myfs()) return -2;
    Superblock* sb = (Superblock*)disk_buf;
    unsigned int table = sb->table_lba;
    unsigned int dptr  = sb->data_ptr;               // запомним: disk_buf станет буфером копирования
    unsigned short fcnt = sb->file_count;
    ata_read_sector(table, sec_buf);
    int vi = -1;                                     // индекс удаляемой записи
    for (int i = 0; i < FS_FILES_MAX; i++) {
        FileEntry* f = (FileEntry*)(sec_buf + i * 64);
        if (f->name[0] && str_eq(f->name, name)) { vi = i; break; }
    }
    if (vi < 0) return -3;
    FileEntry* v = (FileEntry*)(sec_buf + vi * 64);
    unsigned int vsec = (v->size + 511) / 512;       // сколько секторов занимал файл
    // сдвигаем данные всех файлов, лежавших после него
    for (int i = 0; i < FS_FILES_MAX; i++) {
        FileEntry* f = (FileEntry*)(sec_buf + i * 64);
        if (i == vi || f->name[0] == 0 || f->start <= v->start) continue;
        if (vsec > 0) {
            unsigned int fs = (f->size + 511) / 512;
            for (unsigned int s = 0; s < fs; s++) {  // сектор за сектором (копируем «вниз»)
                ata_read_sector(f->start + s, disk_buf);
                ata_write_sector(f->start + s - vsec, disk_buf);
            }
            f->start -= vsec;
        }
    }
    for (int i = 0; i < 64; i++) ((unsigned char*)v)[i] = 0;   // запись удалена
    ata_write_sector(table, sec_buf);
    ata_read_sector(plba + PART_RESERVED, disk_buf); // суперблок: читаем заново после движений
    sb = (Superblock*)disk_buf;
    sb->data_ptr   = dptr - vsec;
    sb->file_count = (unsigned short)(fcnt - 1);
    ata_write_sector(plba + PART_RESERVED, disk_buf);
    return 0;
}

// Переименовать файл oldname → newname в разделе N.
// Возврат: 0 — ок, -1 — нет раздела, -2 — не MyFS, -3 — нет файла, -4 — имя занято.
int fs_rename(int n, const char* oldname, const char* newname) {
    unsigned int plba, psize;
    if (!find_part(n, &plba, &psize)) return -1;
    ata_read_sector(plba + PART_RESERVED, disk_buf);
    if (!is_myfs()) return -2;
    Superblock* sb = (Superblock*)disk_buf;
    ata_read_sector(sb->table_lba, sec_buf);
    FileEntry* found = 0;
    for (int i = 0; i < FS_FILES_MAX; i++) {
        FileEntry* f = (FileEntry*)(sec_buf + i * 64);
        if (f->name[0] == 0) continue;
        if (str_eq(f->name, newname) && !str_eq(f->name, oldname)) return -4;
        if (str_eq(f->name, oldname)) found = f;
    }
    if (!found) return -3;
    for (int i = 0; i < 56; i++) found->name[i] = 0;
    int k = 0;
    while (newname[k] && k < 55) { found->name[k] = newname[k]; k++; }
    ata_write_sector(sb->table_lba, sec_buf);
    return 0;
}

// Показать содержимое файла name из раздела N
void fs_cat(int n, const char* name) {
    unsigned int plba, psize;
    if (!find_part(n, &plba, &psize)) { cmd_print("No such partition\n"); return; }
    ata_read_sector(plba + PART_RESERVED, disk_buf);
    if (!is_myfs()) { cmd_print("Not MyFS (format it first)\n"); return; }
    Superblock* sb = (Superblock*)disk_buf;
    ata_read_sector(sb->table_lba, sec_buf);
    FileEntry* found = 0;
    for (int i = 0; i < FS_FILES_MAX; i++) {
        FileEntry* f = (FileEntry*)(sec_buf + i * 64);
        if (f->name[0] && str_eq(f->name, name)) { found = f; break; }
    }
    if (!found) { cmd_print("No such file\n"); return; }
    unsigned int left = found->size;
    unsigned int lba  = found->start;
    while (left > 0) {
        ata_read_sector(lba, disk_buf);               // суперблок уже не нужен
        int chunk = (left < 512) ? (int)left : 512;
        for (int i = 0; i < chunk; i++) cmd_putchar((char)disk_buf[i]);
        left -= (unsigned int)chunk;
        lba++;
    }
    cmd_putchar('\n');
}

// Записать ДВОИЧНЫЕ данные (ровно len байт) в файл name раздела N.
// Полный аналог fs_write, но длина известна заранее — так ядро возвращает
// на диск shell.bin/demo.bin (из снимков ОЗУ) и копирует их при install.
// Возврат: 0 — ок, -1 — нет раздела, -2 — не MyFS, -3 — имя занято,
//          -4 — таблица полна, -5 — нет места.
int fs_write_buf(int n, const char* name, const unsigned char* data,
                 unsigned int len, bool quiet = false) {
    unsigned int plba, psize;
    if (!find_part(n, &plba, &psize)) {
        if (!quiet) cmd_print("No such partition\n");
        return -1;
    }
    ata_read_sector(plba + PART_RESERVED, disk_buf);
    if (!is_myfs()) {
        if (!quiet) cmd_print("Not MyFS (format it first)\n");
        return -2;
    }
    Superblock* sb = (Superblock*)disk_buf;
    unsigned int table = sb->table_lba;
    unsigned int dptr  = sb->data_ptr;
    ata_read_sector(table, sec_buf);
    int free_idx = -1;
    for (int i = 0; i < FS_FILES_MAX; i++) {
        FileEntry* f = (FileEntry*)(sec_buf + i * 64);
        if (f->name[0] == 0) { if (free_idx < 0) free_idx = i; continue; }
        if (str_eq(f->name, name)) {
            if (!quiet) cmd_print("File exists\n");
            return -3;
        }
    }
    if (free_idx < 0) {
        if (!quiet) cmd_print("File table full\n");
        return -4;
    }
    unsigned int need = (len + 511) / 512;                 // сколько секторов занять
    if (dptr + need > plba + psize) {
        if (!quiet) cmd_print("No space on partition\n");
        return -5;
    }
    sb->data_ptr = dptr + need;                            // суперблок — сразу
    sb->file_count = (unsigned short)(sb->file_count + 1);
    ata_write_sector(plba + PART_RESERVED, disk_buf);
    for (unsigned int s = 0; s < need; s++) {              // данные, сектор за сектором
        for (int i = 0; i < 512; i++) {
            unsigned int pos = s * 512 + (unsigned int)i;
            disk_buf[i] = (pos < len) ? data[pos] : 0;
        }
        ata_write_sector(dptr + s, disk_buf);
    }
    FileEntry* f = (FileEntry*)(sec_buf + free_idx * 64);  // запись в таблице
    for (int i = 0; i < 56; i++) f->name[i] = 0;
    int k = 0;
    while (name[k] && k < 55) { f->name[k] = name[k]; k++; }
    f->start = dptr;
    f->size  = len;
    ata_write_sector(table, sec_buf);
    if (!quiet) {
        cmd_print("Written "); cmd_print(name);
        cmd_print(", "); print_dec(len); cmd_print(" bytes\n");
    }
    return 0;
}
