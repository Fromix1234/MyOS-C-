// ===========================================================================
//  SHELL s12: точка входа — главный цикл оболочки (бывший kernel_main)
// ===========================================================================
//  MyOS v2.0: оболочка — обычная программа ring 3. Всё, что нужно от железа,
//  она просит системными вызовами; рисует напрямую в видеопамять (адрес из
//  SYS_FB, как /dev/fb0). Прерывания, задачи, ввод — забота ядра.
//
//  .text.entry обязан лежать В НАЧАЛЕ образа: ядро прыгает на SHELL_BASE
//  (0x400000) и первая же инструкция должна быть прологом shell_main.
// ===========================================================================

extern "C" __attribute__((section(".text.entry"))) void shell_main() {
    // --- 1) видео: спрашиваем у ядра, где кадровый буфер ---------------------
    init_video();                        // SYS_FB -> fb/W/H/FB_PITCH/FB_BPP/каналы
    init_layout();                       // окно консоли — под текущий экран
    init_desktop_layout();               // док и About — по центру экрана

    // --- 2) состояние окон и мыши (как в v1.2 kernel_main) -------------------
    mouse_ok = 1;                        // ввод идёт из ядра (IRQ1/IRQ12)
    mouse_ack = 0xFA;
    mouse_x = W / 2; mouse_y = H / 2;    // курсор — в центр экрана
    cmd_active = false;
    about_active = false;
    files_active = false;
    set_active = false;
    tasks_active = false;
    game_active = false;
    code_active = false;
    for (int a = 0; a < APP_N; a++) win_z[a] = 0;
    win_focus = -1; win_ztop = 0;
    boot_secs = (int)day_secs();         // время запуска — для «Uptime»
    g_kernel_sectors = (unsigned int)sys_info(0);
    demo_pid = -1;

    draw_desktop();                      // рабочий стол: обои, меню-бар, док
    cursor_show();                       // и стрелка-курсор поверх всего

    // --- 3) главный цикл (как v1.2 + сбор ввода и лог ядра) ------------------
    while (true) {
        input_pump();                    // события ядра -> очередь клавиш/мышь
        log_drain();                     // dmesg ядра -> окно консоли

        if (demo_pid >= 0) {             // работает программа ring 3 (ring3)
            if (!sys_alive(demo_pid)) {
                demo_pid = -1;           // она вышла: возвращаем рабочий стол
                draw_desktop();
                cursor_show();
            }
            sys_yield();                 // демке — больше процессора
            continue;                    // пока она жива, ничего не рисуем
        }

        if (mouse_ok && mouse_read()) {              // пришли данные от мыши
            cursor_hide();
            mouse_handle();
            cursor_show();
            continue;
        }
        int sc = poll_key();
        if (sc < 0) {                                // нет клавиш — «тикаем»
            clock_tick();                            // часы в меню-баре
            if (win_focus == APP_TASKS && tasks_active) tasks_tick();   // «Uptime»
            if (win_focus == APP_GAME  && game_active)  game_tick();    // шаг змейки
            if (win_focus == APP_CODE  && code_active)  code_tick();    // мигание курсора
            sys_yield();                             // дальше — планировщик ядра
            continue;                                // курсор при этом не трогаем
        }

        cursor_hide();
        if (sc == 0x3B) {                            // F1 — переключить окно
            focus_cycle();
            cursor_show();
            continue;
        }
        if (overview_active) {                       // экран Activities ловит всё
            overview_handle_key(sc);
            cursor_show();
            continue;
        }
        int f = win_focus;                           // клавиатура — окну в фокусе
        if      (f == APP_GAME  && game_active)  game_handle_key(sc);
        else if (f == APP_CODE  && code_active)  code_handle_key(sc);
        else if (f == APP_TASKS && tasks_active) tasks_handle_key(sc);
        else if (f == APP_FILES && files_active) files_handle_key(sc);
        else if (f == APP_SET   && set_active)   set_handle_key(sc);
        else if (f == APP_ABOUT && about_active) about_handle_key(sc);
        else if (f == APP_INSTALL && inst_active) inst_handle_key(sc);   // мастер установки
        else if (f == APP_CMD   && cmd_active)   cmd_handle_key(sc);
        else                                     desktop_handle_key(sc);
        cursor_show();
    }
}
