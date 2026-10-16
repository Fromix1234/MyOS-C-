// ===========================================================================
//  SHELL s10: run_command (console command dispatcher)
// ===========================================================================
//  MyOS v2.0 module. Split mechanically from kernel.cpp v1.2
//  (git a81d4f4) and then reworked: see README section 11.

void run_command(const char* line) {
    if (line[0] == 0) return;            // пустая команда
    if (str_eq(line, "help")) {
        cmd_print("Commands:\n");
        cmd_print("  help\n");
        cmd_print("  info\n");
        cmd_print("  clear\n");
        cmd_print("  mem\n");
        cmd_print("  time\n");
        cmd_print("  echo T\n");
        cmd_print("  part\n");
        cmd_print("  mkpart N MB\n");
        cmd_print("  format N\n");
        cmd_print("  ls N\n");
        cmd_print("  write N f T\n");
        cmd_print("  cat N f\n");
        cmd_print("  install N\n");
        cmd_print("  setup\n");
        cmd_print("  play\n");
        cmd_print("  code\n");
        cmd_print("  wtest\n");
        cmd_print("  ring3\n");
        cmd_print("  Up/Dn = history\n");
    } else if (str_eq(line, "play")) {
        game_open();                         // мини-игра «Змейка»
    } else if (str_eq(line, "code")) {
        code_open();                         // редактор кода MyC
    } else if (str_eq(line, "clear")) {
        fill_rect(TXT_X, TXT_Y, TXT_W, TXT_H, C_BLACK);        // снять текст
        cur_col = 0; cur_row = 0;
    } else if (str_eq(line, "info")) {
        cmd_print(MYOS_VERSION " (ring 3 shell)\n");
        cmd_print("Video: VESA VBE (LFB)\n");
        cmd_print("Screen: ");
        print_dec(W); cmd_putchar('x'); print_dec(H); cmd_putchar('x'); print_dec(FB_BPP);
        cmd_print("\n");
        cmd_print("Boot: MBR -> VBR -> stage2\n");
        // Из какого раздела загрузились? Спрашиваем загрузчика — см. boot_partition
        unsigned short bootlba = 0;
        int bp = boot_partition(&bootlba);
        if (bp >= 0) {                       // «записка» от VBR есть
            cmd_print("Boot: P");
            cmd_putchar(bp > 0 ? (char)('0' + bp) : '?');
            cmd_print(" (lba=");
            print_dec(bootlba);
            cmd_print(")\n");
        } else {
            cmd_print("Boot: raw sectors\n");
        }
    } else if (str_eq(line, "mem")) {
        cmd_print("Memory map:\n");
        cmd_print("  Kernel   0x10000\n");
        cmd_print("  Stack    0x90000\n");
        cmd_print("  Video    0x");
        print_hex8((unsigned char)((unsigned int)fb >> 24));   // адрес кадрового
        print_hex8((unsigned char)((unsigned int)fb >> 16));   // буфера — какой
        print_hex8((unsigned char)((unsigned int)fb >> 8));    // дал VESA BIOS
        print_hex8((unsigned char)((unsigned int)fb));
        cmd_print("\n");
        cmd_print("  Conv mem 640 KB\n");
    } else if (str_eq(line, "time")) {
        unsigned int t = day_secs();         // v2.0: время — из ядра (SYS_TIME)
        int sec  = (int)(t % 60);
        int min  = (int)((t / 60) % 60);
        int hour = (int)((t / 3600) % 24);
        cmd_print("Time: ");
        print2(hour); cmd_putchar(':'); print2(min); cmd_putchar(':'); print2(sec);
        cmd_print("\n");
    } else if (str_eq(line, "part")) {
        cmd_part();                          // читаем MBR через ATA и печатаем разделы
    } else if (str_startswith(line, "mkpart ")) {
        // разбираем аргументы: "mkpart N MB"
        const char* p = line + 7;                       // пропускаем слово "mkpart "
        int n = 0, mb = 0;
        while (*p >= '0' && *p <= '9') n = n * 10 + (*p++ - '0');   // номер раздела
        while (*p == ' ') p++;                                      // пробелы
        while (*p >= '0' && *p <= '9') mb = mb * 10 + (*p++ - '0'); // размер, МБ
        cmd_mkpart(n, mb);
    } else if (str_eq(line, "mkpart")) {
        cmd_print("Usage: mkpart N MB  (N=1..4, MB=1..32)\n");
    } else if (str_startswith(line, "format ")) {
        const char* p = skip_word(line);
        fs_format((int)read_num(&p));
    } else if (str_startswith(line, "ls ")) {
        const char* p = skip_word(line);
        fs_ls((int)read_num(&p));
    } else if (str_startswith(line, "write ")) {
        const char* p = skip_word(line);
        int n = (int)read_num(&p);
        char name[24];
        read_word(&p, name, 24);
        if (name[0] == 0) cmd_print("Usage: write N file text\n");
        else fs_write(n, name, p);                    // p — остаток строки (текст)
    } else if (str_startswith(line, "cat ")) {
        const char* p = skip_word(line);
        int n = (int)read_num(&p);
        char name[24];
        read_word(&p, name, 24);
        if (name[0] == 0) cmd_print("Usage: cat N file\n");
        else fs_cat(n, name);
    } else if (str_startswith(line, "install ")) {
        const char* p = skip_word(line);
        int n = (int)read_num(&p);
        char word[8];
        read_word(&p, word, 8);
        cmd_install(n, str_eq(word, "yes"));   // без "yes" — только план, ничего не пишем
    } else if (str_eq(line, "install")) {
        cmd_print("Usage: install N [yes]\n");
    } else if (str_eq(line, "setup")) {
        inst_open();                         // графический мастер установки
    } else if (str_eq(line, "ring3")) {
        // v2.0: demo.bin — отдельная ЗАДАЧА ядра (ring 3, 5..6 МБ). Ядро само
        // копирует её из снимка ОЗУ и запускает; оболочка только ждёт возврата.
        if (demo_pid < 0) {
            demo_pid = sys_spawn("demo.bin", DEMO_BASE);
            if (demo_pid < 0) cmd_print("ring3: cannot start demo.bin\n");
        } else {
            cmd_print("ring3: demo is already running\n");
        }
    } else if (str_eq(line, "wtest")) {
        cmd_wtest();                         // тест записи: write + read + сверка
    } else if (str_startswith(line, "echo ")) {
        cmd_print(line + 5);
    } else if (str_eq(line, "echo")) {
        cmd_print("(nothing to echo)\n");
    } else {
        cmd_print("Unknown: ");
        cmd_print(line);
        cmd_print("\n");
    }
}
