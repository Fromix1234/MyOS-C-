// ===========================================================================
//  KERNEL k4b: installer core (check/plan/do)
// ===========================================================================
//  MyOS v2.0 module. Split mechanically from kernel.cpp v1.2
//  (git a81d4f4) and then reworked: see README section 11.

// ===========================================================================
//  УСТАНОВКА СИСТЕМЫ В РАЗДЕЛ — команда install N [yes]
// ===========================================================================
//  Копирует систему из НАЧАЛА диска (откуда она собиралась) в служебную
//  зону раздела N (первые PART_RESERVED секторов):
//    сектор 1 диска (VBR)      -> +0  раздела
//    сектора 2-3 диска (stage2)-> +1  раздела (2 сектора)
//    сектора 4+ диска (ядро)   -> +3  раздела (KERNEL_SECTORS штук)
//
//  VBR не знает, в каком разделе лежит, поэтому установщик ВПЕЧАТЫВАЕТ в его
//  копию LBA раздела — 4 байта по смещению 504 (поле base_lba, см. vbr.asm).
//  По этому полю VBR (и потом stage2) находят свои stage2 и ядро.
//
//  БЕЗОПАСНОСТЬ: без слова yes ничего не пишем — сначала показываем план.
//  MyFS живёт с сектора +PART_RESERVED (192), а install пишет только начало —
//  файлы не задеваются.
// Проверить раздел для установки; при отказе печатает причину.
// true = можно ставить, *plba/*psize заполнены.
bool install_check(int n, unsigned int* plba, unsigned int* psize) {
    if (!find_part(n, plba, psize)) { cmd_print("No such partition\n"); return false; }
    if (*psize < PART_RESERVED + 3) { cmd_print("Partition too small\n"); return false; }
    if (KERNEL_SECTORS + 3 > PART_RESERVED) {
        cmd_print("Kernel too big for boot area\n"); return false;
    }
    return true;
}

// Напечатать план установки в раздел n (общий код для install и setup)
void install_plan(int n, unsigned int plba, unsigned int psize) {
    cmd_print("Install to P");
    cmd_putchar((char)('0' + n));
    cmd_print(": lba=");  print_dec(plba);
    cmd_print(" size=");  print_dec(psize / 2048); cmd_print("MB\n");
    cmd_print("Boot area: first "); print_dec(PART_RESERVED); cmd_print(" sec\n");
    cmd_print("  boot 1 + stage2 2 + kernel ");
    print_dec(KERNEL_SECTORS);
    cmd_print("\n");
    cmd_print("MyFS files stay untouched.\n");
}

// Сама установка (без проверок и вопросов): копируем и помечаем активным.
// talk = false — не печатать шаги в консоль (мастер показывает их в окне).
void install_do(int n, unsigned int plba, bool talk = true) {
    if (talk) {
        cmd_print("Installing to P");
        cmd_putchar((char)('0' + n));
        cmd_print("...\n");
    }

    // --- 1. VBR: сектор 1 диска + впечатать LBA раздела (поле на +504) ---
    ata_read_sector(1, disk_buf);
    disk_buf[504] = (unsigned char)(plba & 0xFF);      // LBA раздела (u32, LE)
    disk_buf[505] = (unsigned char)((plba >> 8) & 0xFF);
    disk_buf[506] = (unsigned char)((plba >> 16) & 0xFF);
    disk_buf[507] = (unsigned char)((plba >> 24) & 0xFF);
    ata_write_sector(plba, disk_buf);
    if (talk) { cmd_print("  VBR -> "); print_dec(plba); cmd_print("\n"); }

    // --- 2. stage2 (2 сектора): сектора 2-3 диска -> +1..+2 раздела ---
    ata_read_sector(2, disk_buf);
    ata_write_sector(plba + 1, disk_buf);
    ata_read_sector(3, disk_buf);
    ata_write_sector(plba + 2, disk_buf);
    if (talk) { cmd_print("  stage2 2 sec -> "); print_dec(plba + 1); cmd_print("\n"); }

    // --- 3. ядро: сектора 4+ диска -> +3.. раздела (читаем и пишем посекторно) ---
    for (unsigned int i = 0; i < KERNEL_SECTORS; i++) {
        ata_read_sector(4 + i, disk_buf);
        ata_write_sector(plba + 3 + i, disk_buf);
    }
    if (talk) {
        cmd_print("  kernel "); print_dec(KERNEL_SECTORS);
        cmd_print(" sec -> "); print_dec(plba + 3); cmd_print("\n");
    }

    // --- 4. Раздел N становится АКТИВНЫМ (флаг 0x80), остальные снимаем ---
    ata_read_sector(0, disk_buf);
    for (int i = 0; i < 4; i++) {
        unsigned char* e = disk_buf + 446 + i * 16;
        if (e[4] == 0) continue;                       // пустой слот не трогаем
        e[0] = (i == n - 1) ? 0x80 : 0x00;
    }
    ata_write_sector(0, disk_buf);

    if (talk) {
        cmd_print("P"); cmd_putchar((char)('0' + n));
        cmd_print(" marked active. Done.\n");
    }
}

// Команда install N [yes]: без yes — только план, с yes — установка
void cmd_install(int n, bool confirm) {
    unsigned int plba, psize;
    if (!install_check(n, &plba, &psize)) return;
    install_plan(n, plba, psize);
    if (!confirm) {
        cmd_print("Type: install ");
        cmd_putchar((char)('0' + n));
        cmd_print(" yes\n");
        return;
    }
    install_do(n, plba);
}
