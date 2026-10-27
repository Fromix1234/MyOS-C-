// ===========================================================================
//  KERNEL k10: точка входа — поднять железо, загрузить ОБОЛОЧКУ с диска
//              и отдать ей машину (ядро дальше только обслуживает сисколлы)
// ===========================================================================
//  Порядок загрузки (как в Linux: ядро + userland с диска):
//    serial -> видео («записка» stage2) -> страницы -> GDT/TSS -> IDT ->
//    PIC -> таймер -> PS/2 -> прерывания вкл -> заставка -> чтение shell.bin
//    из MyFS-раздела -> первая задача (ring 3) -> холостой цикл.
//
//  Снимки системы (shell.bin/demo.bin) лежат в ОЗУ: из них SYS_SPAWN берёт
//  demo, а SYS_FS_FORMAT / SYS_INSTALL восстанавливают файлы системы.
// ===========================================================================

// Экран ошибки загрузки (рисуем шрифтом ядра — оболочки-то нет)
void boot_error_screen(const char* what) {
    fill_rect(0, 0, W, H, 0x000000);
    draw_text(24, 40, "MyOS: boot error", 0xFFFFFF);
    draw_text(24, 70, what, 0xFFFFFF);
    draw_text(24, 100, "The system partition must be MyFS with shell.bin.", 0xFFFFFF);
    for (;;) asm volatile("hlt");
}

extern "C" void kernel_main() {
    serial_init();                      // COM1 — ещё до всего (тесты, отладка)
    log_reset();
    log_puts("boot: kernel start (MyOS v3.1 pre-release)\n");   // метки загрузки: dmesg + COM1
    init_video();                       // видео: «записка» stage2 (VESA VBE)
    log_puts("video: note=0x"); log_hex(*(volatile unsigned short*)0x0510);
    log_puts(" fb=0x");  log_hex((unsigned int)fb);
    log_puts(" w=0x");   log_hex((unsigned int)W);
    log_puts(" h=0x");   log_hex((unsigned int)H);
    log_puts(" bpp=0x"); log_hex((unsigned int)FB_BPP);
    log_puts(" pitch=0x"); log_hex((unsigned int)FB_PITCH);
    log_puts("\n");
    mem_init((unsigned int)fb);         // страницы: ядро 0..4 МБ, программы 4..8 МБ
    log_puts("mem: pd0=0x"); log_hex(page_dir[0]);
    log_puts(" pd1=0x"); log_hex(page_dir[1]);
    log_puts(" pd2=0x"); log_hex(page_dir[2]);
    log_puts(" cr3=0x"); log_hex((unsigned int)page_dir);
    log_puts(" pd896=0x"); log_hex(page_dir[896]);
    log_puts("\n");
    gdt_init();                         // сегменты ring 0/3 + TSS (как в Linux)
    idt_init();                         // шлюз int 0x80 + IRQ0/1/12 + ловушки
    pic_init();                         // контроллер прерываний (IRQ0/1/12)
    pit_init();                         // системный таймер 100 Гц
    ps2_init();                         // клавиатура и мышь (IRQ1/IRQ12)
    // Явное обнуление состояния (часть .bss может не попасть в плоский образ)
    kbd_reset(); mouse_reset();
    timer_ticks = 0; timer_slice = 0;
    for (int i = 0; i < TASK_MAX; i++) tasks[i].state = 0;
    task_count = 0; cur_task = 0; input_owner = 1;
    asm volatile("sti");                // тики и ввод пошли
    // Гасим «залётные» ACK-байты PS/2 (0xFA): к моменту sti один такой может
    // ещё лежать в выходном буфере контроллера, первый же IRQ1 кладёт его в
    // очередь клавиатуры — и заставка/первый ввод принимают его за нажатие
    // (проверено отладкой: splash-exit key=FA wait=0ms). Вычитываем буфер
    // ~30 мс по тикам PIT и сбрасываем очереди.
    unsigned int t0 = millis();
    while (millis() - t0 < 30) {
        while (inb(0x64) & 0x01) inb(0x60);      // слить остатки из буфера
        asm volatile("pause");
    }
    kbd_reset(); mouse_reset();
    draw_splash();                      // заставка ~1.4 с (клавиша пропускает)

    // TEMP DEBUG: файлы грузим с запрещёнными прерываниями, чтобы понять,
    // виноват ли IRQ-обработчик в падении при чтении shell.bin.
    asm volatile("cli");

    // --- Где система? «Записка» VBR (0x0500/0x0502), иначе — перебор ---------
    // Если stage2 уже положил shell.bin в ОЗУ (загрузка с ISO/флоппи), читаем
    // его оттуда: на CD нет ATA-диска, и MyFS-код туда не полезет.
    unsigned int pre_magic = *(volatile unsigned int*)0x0520;
    unsigned int pre_size  = *(volatile unsigned int*)0x0524;
    unsigned int pre_addr  = *(volatile unsigned int*)0x0528;
    unsigned int pre_drive = *(volatile unsigned int*)0x052C;
    bool cd_preload = (pre_magic == 0x504C4443u && pre_addr == 0x00030000u &&
                       pre_size > 0 && pre_size <= 0x100000u);
    g_cd_preload = cd_preload ? 1 : 0;

    unsigned short blba = 0;
    int sh = -1;
    int dl = -1;
    if (cd_preload) {
        log_puts("boot: ISO preload size=0x"); log_hex(pre_size);
        log_puts(" drive=0x"); log_hex(pre_drive); log_puts("\n");
        unsigned int n4 = pre_size & ~3u;
        for (unsigned int i = 0; i < n4; i += 4)
            *(unsigned int*)(shell_snap + i) = *(unsigned int*)(pre_addr + i);
        for (unsigned int i = n4; i < pre_size; i++)
            shell_snap[i] = ((unsigned char*)pre_addr)[i];
        sh = (int)pre_size;
        sys_part = 0;                   // на ISO нет MyFS-раздела
        dl = 0;
    } else {
        int bp = boot_partition(&blba);
        sys_part = (bp >= 1) ? bp : 0;
        if (sys_part > 0)
            sh = fs_read_by_name(sys_part, "shell.bin", shell_snap, 0x100000);
        if (sh <= 0) {                  // раздел без файлов системы — ищем сами
            int n2 = find_part_with_file("shell.bin");
            if (n2 > 0) {
                sys_part = n2;
                sh = fs_read_by_name(sys_part, "shell.bin", shell_snap, 0x100000);
            }
        }
        if (sh <= 0) boot_error_screen("shell.bin not found on any MyFS partition");
        dl = fs_read_by_name(sys_part, "demo.bin", demo_snap, 0x100000);
    }
    shell_snap_len = (unsigned int)sh;
    demo_snap_len = (dl > 0) ? (unsigned int)dl : 0;
    log_puts("boot: sys_part=0x"); log_hex((unsigned int)sys_part);
    log_puts(" sh=0x"); log_hex((unsigned int)sh);
    log_puts(" dl=0x"); log_hex((unsigned int)dl);
    log_puts(" shell.bin loaded\n");
    asm volatile("sti");

    // --- Запускаем ОБОЛОЧКУ как первую задачу (ring 3) -----------------------
    for (unsigned int i = 0; i < 0x100000u; i += 4)          // чистая зона 4..5 МБ
        *(volatile unsigned int*)(SHELL_BASE + i) = 0;
    for (unsigned int i = 0; i < shell_snap_len; i++)
        ((unsigned char*)SHELL_BASE)[i] = shell_snap[i];
    int pid = task_create("shell", SHELL_BASE, SHELL_STACK_TOP, 0);
    if (pid < 1) boot_error_screen("cannot create shell task");
    input_owner = pid;
    cur_task = pid - 1;
    tss.esp0 = tasks[pid - 1].kstack_top;
    log_puts("boot: task esp=0x"); log_hex(tasks[pid - 1].esp);
    log_puts(" kstack=0x"); log_hex(tasks[pid - 1].kstack_top);
    log_puts(" entry=0x"); log_hex((unsigned int)SHELL_BASE);
    log_puts(" entering shell (ring 3)\n");
    jump_to_task(tasks[pid - 1].esp);   // надеть контекст оболочки — и в ring 3
    for (;;) asm volatile("hlt");       // сюда уже не попадём — холостой резерв
}
