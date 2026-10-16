// ===========================================================================
//  KERNEL k4: partitions (part/wtest/mkpart commands)
// ===========================================================================
//  MyOS v2.0 module. Split mechanically from kernel.cpp v1.2
//  (git a81d4f4) and then reworked: see README section 11.

void cmd_part() {
    ata_read_sector(0, disk_buf);
    if (disk_buf[510] != 0x55 || disk_buf[511] != 0xAA) {
        cmd_print("No MBR signature (disk is not ATA?)\n");
        return;
    }
    cmd_print("MBR partitions:\n");
    for (int i = 0; i < 4; i++) {
        unsigned char* e = disk_buf + 446 + i * 16;      // запись раздела i
        if (e[4] == 0) continue;                         // пустая запись — пропуск
        unsigned int lba = (unsigned int)e[8]  | ((unsigned int)e[9]  << 8) |
                           ((unsigned int)e[10] << 16) | ((unsigned int)e[11] << 24);
        unsigned int sec = (unsigned int)e[12] | ((unsigned int)e[13] << 8) |
                           ((unsigned int)e[14] << 16) | ((unsigned int)e[15] << 24);
        cmd_print("  P");
        cmd_putchar((char)('1' + i));
        cmd_print(" type=");
        print_hex8(e[4]);
        cmd_print(" lba=");
        print_dec(lba);
        cmd_print(" size=");
        print_dec(sec / 2048);                           // 2048 секторов = 1 МБ
        cmd_print("MB\n");
    }
}

// Команда wtest: проверяем ЗАПИСЬ на диск.
// Пишем в свободный сектор узор, стираем буфер, читаем обратно и сверяем.
void cmd_wtest() {
    const unsigned int LBA = 60000;              // свободный сектор (после разделов)
    cmd_print("Write test @ LBA ");
    print_dec(LBA);
    cmd_print("...\n");
    // 1. Заполняем буфер узором: байт i = i*7+13 (перестановка байт сразу заметна)
    for (int i = 0; i < 512; i++) disk_buf[i] = (unsigned char)(i * 7 + 13);
    // 2. Пишем сектор на диск (ATA WRITE SECTORS + FLUSH)
    ata_write_sector(LBA, disk_buf);
    // 3. Стираем буфер — чтобы обратное чтение было ЧЕСТНЫМ (не остатки в памяти)
    for (int i = 0; i < 512; i++) disk_buf[i] = 0;
    // 4. Читаем сектор обратно
    ata_read_sector(LBA, disk_buf);
    // 5. Сверяем байт за байтом
    int bad = -1;
    for (int i = 0; i < 512; i++)
        if (disk_buf[i] != (unsigned char)(i * 7 + 13)) { bad = i; break; }
    if (bad < 0) {
        cmd_print("  OK - 512 bytes match\n");
    } else {
        cmd_print("  FAIL at byte ");
        print_dec((unsigned int)bad);
        cmd_print("\n");
    }
}

// Команда mkpart: создать (или изменить) раздел N размером MB мегабайт.
// Пишем ТОЛЬКО байты 446..509 сектора 0 — загрузочный код в начале не трогаем!
void cmd_mkpart(int n, int mb) {
    if (n < 1 || n > 4) {
        cmd_print("Usage: mkpart N MB  (N=1..4)\n");
        return;
    }
    if (mb < 1 || mb > 32) {
        cmd_print("Size must be 1..32 MB\n");
        return;
    }
    ata_read_sector(0, disk_buf);                    // читаем сектор 0 (код + таблица)
    if (disk_buf[510] != 0x55 || disk_buf[511] != 0xAA) {
        cmd_print("No MBR signature\n");
        return;
    }
    unsigned char* e = disk_buf + 446 + (n - 1) * 16;   // запись раздела N
    unsigned int start;
    if (e[4] != 0) {
        // слот уже занят — оставляем его начало, меняем только размер
        start = (unsigned int)e[8]  | ((unsigned int)e[9]  << 8) |
                ((unsigned int)e[10] << 16) | ((unsigned int)e[11] << 24);
    } else {
        // слот пуст — ставим новый раздел после самого дальнего занятого
        start = 2048;                                // минимум: 1 МБ от начала диска
        for (int i = 0; i < 4; i++) {
            unsigned char* o = disk_buf + 446 + i * 16;
            if (o[4] == 0) continue;                 // пустая запись — пропуск
            unsigned int olba = (unsigned int)o[8]  | ((unsigned int)o[9]  << 8) |
                                ((unsigned int)o[10] << 16) | ((unsigned int)o[11] << 24);
            unsigned int osec = (unsigned int)o[12] | ((unsigned int)o[13] << 8) |
                                ((unsigned int)o[14] << 16) | ((unsigned int)o[15] << 24);
            if (olba + osec > start) start = olba + osec;
        }
        start = ((start + 2047) / 2048) * 2048;      // выравниваем начало на 1 МБ
    }
    unsigned int size = (unsigned int)mb * 2048;     // размер: МБ -> сектора
    if (start + size > DISK_SECTORS) {               // не вылезаем за диск
        cmd_print("No space: only ");
        print_dec((DISK_SECTORS - start) / 2048);
        cmd_print("MB free\n");
        return;
    }
    // Заполняем запись раздела (16 байт) — как в настоящем MBR
    if (e[4] == 0) e[0] = 0;                         // новый раздел — не загрузочный
    e[1] = 0; e[2] = 0; e[3] = 0;                    // CHS начала (для LBA не нужен)
    e[4] = 0x83;                                     // тип раздела: MyOS
    e[5] = 0; e[6] = 0; e[7] = 0;                    // CHS конца (для LBA не нужен)
    e[8]  = (unsigned char)(start & 0xFF);           // LBA начала (4 байта)
    e[9]  = (unsigned char)((start >> 8) & 0xFF);
    e[10] = (unsigned char)((start >> 16) & 0xFF);
    e[11] = (unsigned char)((start >> 24) & 0xFF);
    e[12] = (unsigned char)(size & 0xFF);            // размер в секторах (4 байта)
    e[13] = (unsigned char)((size >> 8) & 0xFF);
    e[14] = (unsigned char)((size >> 16) & 0xFF);
    e[15] = (unsigned char)((size >> 24) & 0xFF);
    // Пишем сектор 0 обратно и сразу же показываем таблицу (контрольное чтение)
    ata_write_sector(0, disk_buf);
    cmd_print("Created P");
    cmd_putchar((char)('0' + n));
    cmd_print(": lba=");
    print_dec(start);
    cmd_print(" size=");
    print_dec((unsigned int)mb);
    cmd_print("MB\n");
    cmd_part();
}
