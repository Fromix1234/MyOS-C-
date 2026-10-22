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

// Rust-компонент ядра (kernel-rs/rs_probe.rs): проверка сборки и C ABI.
extern "C" void rs_probe();

extern "C" void kernel_main() {
    serial_init();                      // COM1 — ещё до всего (тесты, отладка)
    log_reset();
    log_puts("boot: kernel start (MyOS v2.0)\n");   // метки загрузки: dmesg + COM1
    rs_probe();                         // прототип: Rust в ring 0 (см. kernel-rs)
    init_video();                       // видео: «записка» stage2 (VESA VBE)
    mem_init((unsigned int)fb);         // страницы: ядро 0..4 МБ, программы 4..8 МБ
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

    // --- Где система? «Записка» VBR (0x0500/0x0502), иначе — перебор ---------
    unsigned short blba = 0;
    int bp = boot_partition(&blba);
    sys_part = (bp >= 1) ? bp : 0;
    int sh = -1;
    if (sys_part > 0)
        sh = fs_read_by_name(sys_part, "shell.bin", shell_snap, 0x100000);
    if (sh <= 0) {                      // раздел без файлов системы — ищем сами
        int n2 = find_part_with_file("shell.bin");
        if (n2 > 0) {
            sys_part = n2;
            sh = fs_read_by_name(sys_part, "shell.bin", shell_snap, 0x100000);
        }
    }
    if (sh <= 0) boot_error_screen("shell.bin not found on any MyFS partition");
    shell_snap_len = (unsigned int)sh;
    int dl = fs_read_by_name(sys_part, "demo.bin", demo_snap, 0x100000);
    demo_snap_len = (dl > 0) ? (unsigned int)dl : 0;
    log_puts("boot: shell.bin loaded\n");

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
    log_puts("boot: entering shell (ring 3)\n");
    jump_to_task(tasks[pid - 1].esp);   // надеть контекст оболочки — и в ring 3
    for (;;) asm volatile("hlt");       // сюда уже не попадём — холостой резерв
}
