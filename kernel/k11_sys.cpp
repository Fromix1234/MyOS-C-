// ===========================================================================
//  KERNEL k11: системные вызовы — единственная дверь из ring 3 в ring 0
// ===========================================================================
//  Договор (см. common/myos.h): eax = номер, ebx/ecx/edx/esi/edi = аргументы,
//  ответ в eax. Каждый указатель проверяется user_ok — программа не может
//  попросить ядро «почитать» память ядра (как copy_from_user в Linux).
// ===========================================================================

// --- Системные файлы (shell.bin/demo.bin) неприкосновенны ------------------
bool is_sys_file(const char* name) {
    const char* a = name;
    const char* b = "shell.bin";
    while (*a && *a == *b) { a++; b++; }
    if (*a == *b) return true;              // оба закончились — совпало
    a = name; b = "demo.bin";
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

// Если раздел n — MyFS, положить на него файлы системы из снимков ОЗУ
// (используется после install; формат делает то же самое в SYS_FS_FORMAT).
void ensure_sys_files(int n) {
    int cnt = fs_load_table(n);
    if (cnt < 0) return;                    // нет раздела или не MyFS — не трогаем
    bool has_shell = false, has_demo = false;
    for (int i = 0; i < cnt; i++) {
        if (is_sys_file(file_list[i].name)) {
            if (file_list[i].name[0] == 's') has_shell = true;
            else has_demo = true;
        }
    }
    if (!has_shell && shell_snap_len > 0)
        fs_write_buf(n, "shell.bin", shell_snap, shell_snap_len, true);
    if (!has_demo && demo_snap_len > 0)
        fs_write_buf(n, "demo.bin", demo_snap, demo_snap_len, true);
}

// --- Таблица разделов для мастера установки (как inst_scan в v1.2) ---------
int sys_part_list(myos_part_t* out) {
    ata_read_sector(0, disk_buf);
    if (disk_buf[510] != 0x55 || disk_buf[511] != 0xAA) {
        cmd_print("No MBR signature (disk is not ATA?)\n");
        return 0;
    }
    int cnt = 0;
    for (int i = 0; i < 4; i++) {
        out[i].lba = 0; out[i].sectors = 0; out[i].flags = 0;
        unsigned char* e = disk_buf + 446 + i * 16;
        if (e[4] == 0) continue;
        unsigned int plba = (unsigned int)e[8]  | ((unsigned int)e[9]  << 8) |
                            ((unsigned int)e[10] << 16) | ((unsigned int)e[11] << 24);
        unsigned int psize = (unsigned int)e[12] | ((unsigned int)e[13] << 8) |
                             ((unsigned int)e[14] << 16) | ((unsigned int)e[15] << 24);
        unsigned int fl = 1;                        // слот занят
        if (e[0] == 0x80) fl |= 8;                  // он же активный
        if (psize >= PART_RESERVED + 3 && KERNEL_SECTORS + 3 <= PART_RESERVED) fl |= 4;
        sec_buf[0] = 0;                             // чтобы мусор не дал ложный MyFS
        ata_read_sector(plba + PART_RESERVED, sec_buf);
        if (sec_buf[0] == 'M' && sec_buf[1] == 'Y' &&
            sec_buf[2] == 'F' && sec_buf[3] == 'S') fl |= 2;
        out[i].lba = plba;
        out[i].sectors = psize;
        out[i].flags = fl;
        cnt++;
    }
    return cnt;
}

// --- Сам диспетчер (номера — из common/myos.h) -----------------------------
unsigned int sys_dispatch(unsigned int num, unsigned int a1, unsigned int a2,
                          unsigned int a3, unsigned int a4, unsigned int a5) {
    if (num == SYS_EXIT) {                                  // 0
        cmd_print("ring3: back in the kernel (ring 0).\n");
        task_exit(task_pid());
        return 0;                                           // сюда не вернёмся
    }
    if (num == SYS_KEY) {                                   // 1
        if (task_pid() != input_owner) return (unsigned int)-1;
        return (unsigned int)poll_key();
    }
    if (num == SYS_TIME) return day_secs();                 // 2
    if (num == SYS_TICKS) return millis();                  // 3
    if (num == SYS_PID) return (unsigned int)task_pid();    // 4
    if (num == SYS_FB) {                                    // 5
        if (!user_ok(a1, sizeof(myos_fb_t))) return (unsigned int)-1;
        myos_fb_t* p = (myos_fb_t*)a1;
        p->addr = (unsigned int)fb;
        p->w = W; p->h = H; p->bpp = FB_BPP; p->pitch = FB_PITCH;
        p->rpos = FB_RPOS; p->gpos = FB_GPOS; p->bpos = FB_BPOS;
        return 0;
    }
    if (num == SYS_PRINT) {                                 // 6
        char buf[256];
        int n = copy_str_from_user(buf, (const char*)a1, 256);
        if (n < 0) return (unsigned int)-1;
        log_puts(buf);
        return (unsigned int)n;
    }
    if (num == SYS_LOG_READ) {                              // 7
        if (!user_ok(a1, 1)) return (unsigned int)-1;
        unsigned int max = a2;
        unsigned int room = 0x00800000u - a1;               // сколько влезет до границы
        if (max > room) max = room;
        if (max > 4096) max = 4096;
        return log_read((char*)a1, max);
    }
    if (num == SYS_FS_LIST) {                               // 8 (n, out, max)
        int max = (int)a3;
        if (max > FS_ENT_MAX) max = FS_ENT_MAX;
        if (max <= 0 || !user_ok(a2, (unsigned int)max * sizeof(myos_file_t)))
            return (unsigned int)-1;
        int cnt = fs_load_table((int)a1);
        if (cnt < 0) return (unsigned int)cnt;
        if (cnt > max) cnt = max;
        myos_file_t* out = (myos_file_t*)a2;
        for (int i = 0; i < cnt; i++) {
            int k = 0;
            while (k < 55 && file_list[i].name[k]) { out[i].name[k] = file_list[i].name[k]; k++; }
            out[i].name[k] = 0;
            out[i].start = file_list[i].start;
            out[i].size  = file_list[i].size;
        }
        return (unsigned int)cnt;
    }
    if (num == SYS_FS_READ) {                               // 9 (n, name, dst, max)
        char nm[64];
        if (copy_str_from_user(nm, (const char*)a2, 64) < 0) return (unsigned int)-1;
        if (!user_ok(a3, 1)) return (unsigned int)-1;
        unsigned int max = a4;
        unsigned int room = 0x00800000u - a3;
        if (max > room) max = room;
        return (unsigned int)fs_read_by_name((int)a1, nm, (unsigned char*)a3, max);
    }
    if (num == SYS_FS_READAT) {                             // 10 (start, size, dst, max)
        if (!user_ok(a3, 1)) return (unsigned int)-1;
        unsigned int max = a4;
        unsigned int room = 0x00800000u - a3;
        if (max > room) max = room;
        return (unsigned int)fs_read_at(a1, a2, (unsigned char*)a3, max);
    }
    if (num == SYS_FS_WRITE) {                              // 11 (n, name, data, len, quiet)
        char nm[64];
        if (copy_str_from_user(nm, (const char*)a2, 64) < 0) return (unsigned int)-1;
        int quiet = (a5 != 0);
        if ((int)a4 < 0)                                    // текст до нуля (как v1.2)
            return (unsigned int)fs_write((int)a1, nm, (const char*)a3, quiet);
        if (!user_ok(a3, a4)) return (unsigned int)-1;
        return (unsigned int)fs_write_buf((int)a1, nm, (const unsigned char*)a3, a4, quiet);
    }
    if (num == SYS_FS_DELETE) {                             // 12
        char nm[64];
        if (copy_str_from_user(nm, (const char*)a2, 64) < 0) return (unsigned int)-1;
        if (is_sys_file(nm)) return (unsigned int)SYS_ERR_SYSFIL;
        return (unsigned int)fs_delete((int)a1, nm);
    }
    if (num == SYS_FS_RENAME) {                             // 13
        char o[64], nw[64];
        if (copy_str_from_user(o, (const char*)a2, 64) < 0) return (unsigned int)-1;
        if (copy_str_from_user(nw, (const char*)a3, 64) < 0) return (unsigned int)-1;
        if (is_sys_file(o) || is_sys_file(nw)) return (unsigned int)SYS_ERR_SYSFIL;
        return (unsigned int)fs_rename((int)a1, o, nw);
    }
    if (num == SYS_FS_FORMAT) {                             // 14
        int r = fs_format((int)a1, a2 != 0);                // a2 — «тихо» (окно Files)
        if (r != 0) return (unsigned int)r;
        if ((int)a1 == sys_part) {                          // раздел системы — вернуть файлы
            if (shell_snap_len > 0)
                fs_write_buf((int)a1, "shell.bin", shell_snap, shell_snap_len, true);
            if (demo_snap_len > 0)
                fs_write_buf((int)a1, "demo.bin", demo_snap, demo_snap_len, true);
        }
        return 0;
    }
    if (num == SYS_PART_LIST) {                             // 15
        if (!user_ok(a1, 4 * sizeof(myos_part_t))) return (unsigned int)-1;
        return (unsigned int)sys_part_list((myos_part_t*)a1);
    }
    if (num == SYS_PART_MK) {                               // 16 (n, mb)
        cmd_mkpart((int)a1, (int)a2);
        return 0;
    }
    if (num == SYS_DISK_WTEST) { cmd_wtest(); return 0; }   // 17
    if (num == SYS_INSTALL_PLAN) {                          // 18 (n, out)
        if (!user_ok(a2, sizeof(myos_install_t))) return (unsigned int)-1;
        myos_install_t* out = (myos_install_t*)a2;
        out->lba = 0; out->sectors = 0;
        out->kern_sectors = KERNEL_SECTORS;
        unsigned int plba = 0, psize = 0;
        if (!install_check((int)a1, &plba, &psize)) { out->code = -1; return (unsigned int)-1; }
        out->lba = plba;
        out->sectors = psize;
        out->code = 0;
        return 0;
    }
    if (num == SYS_INSTALL) {                               // 19 (n, flags)
        // flags: bit0 — подтверждение («yes»: ПИСАТЬ), bit1 — тихо (без печати
        // плана: графический мастер показывает план сам в своём окне).
        unsigned int plba = 0, psize = 0;
        if (!install_check((int)a1, &plba, &psize)) return (unsigned int)-1;
        if (!(a2 & 2))
            install_plan((int)a1, plba, psize);             // перед записью — ВСЕГДА план
        if (!(a2 & 1)) {                                    // без подтверждения не пишем
            cmd_print("Type: install ");
            cmd_putchar((char)('0' + (int)a1));
            cmd_print(" yes\n");
            return 0;
        }
        install_do((int)a1, plba, (a2 & 2) == 0);           // копирует зону загрузки
        ensure_sys_files((int)a1);                          // и файлы системы (если MyFS)
        return 0;
    }
    if (num == SYS_BOOTPART) {                              // 20
        if (!user_ok(a1, sizeof(myos_boot_t))) return (unsigned int)-1;
        unsigned short l = 0;
        int idx = boot_partition(&l);
        myos_boot_t* b = (myos_boot_t*)a1;
        b->index = idx;
        b->lba = l;
        return (unsigned int)idx;
    }
    if (num == SYS_SPAWN) {                                 // 21 (name, base)
        char nm[64];
        if (copy_str_from_user(nm, (const char*)a1, 64) < 0) return (unsigned int)-1;
        if (a2 < SHELL_BASE || a2 > USER_TOP - 0x10000) return (unsigned int)-1;
        if (sys_part == 0) return (unsigned int)-1;
        // зона программы — чистая (как mmap со свежими страницами)
        for (unsigned int i = 0; i < 0x100000u; i += 4)
            *(volatile unsigned int*)(a2 + i) = 0;
        int len = fs_read_by_name(sys_part, nm, (unsigned char*)a2, 0x100000);
        if (len <= 0) return (unsigned int)-1;
        cmd_print("ring3: entering user mode...\n");
        int pid = task_create(nm, a2, a2 + 0xFFFF0u, task_pid());
        if (pid < 0) return (unsigned int)-1;
        input_owner = pid;                                  // клавиатура — программе
        return (unsigned int)pid;
    }
    if (num == SYS_ALIVE) return task_alive((int)a1) ? 1 : 0;   // 22
    if (num == SYS_INFO) {                                  // 23
        if (a1 == 0) return (unsigned int)KERNEL_SECTORS;
        if (a1 == 1) return PART_RESERVED;
        return 0;
    }
    if (num == SYS_INPUT) {                                 // 24 (events, max)
        unsigned int max = a2;
        if (max > 64) max = 64;
        if (max == 0) return 0;
        if (!user_ok(a1, max * sizeof(myos_event_t))) return (unsigned int)-1;
        myos_event_t* out = (myos_event_t*)a1;
        bool owner = (task_pid() == input_owner);
        int cnt = 0;
        while ((unsigned int)cnt < max) {
            if (owner) {
                int sc = poll_key();
                if (sc >= 0) {
                    out[cnt].type = EV_KEY;
                    out[cnt].d1 = sc; out[cnt].d2 = 0; out[cnt].d3 = 0;
                    cnt++;
                    continue;
                }
            }
            int dx, dy, btn;
            if (mouse_poll(&dx, &dy, &btn)) {
                out[cnt].type = EV_MOUSE;
                out[cnt].d1 = dx; out[cnt].d2 = dy; out[cnt].d3 = btn;
                cnt++;
                continue;
            }
            break;
        }
        return (unsigned int)cnt;
    }
    if (num == SYS_FS_LS) { fs_ls((int)a1); return 0; }     // 25
    if (num == SYS_FS_CAT) {                                // 26
        char nm[64];
        if (copy_str_from_user(nm, (const char*)a2, 64) < 0) return (unsigned int)-1;
        fs_cat((int)a1, nm);
        return 0;
    }
    if (num == SYS_PART_LS) { cmd_part(); return 0; }       // 27
    if (num == SYS_YIELD) { schedule_now(); return 0; }     // 28
    return (unsigned int)-1;                                // неизвестный вызов
}

// --- Мостик из ассемблера: кадр лежит в порядке из k7b ---------------------
// Стаб пушит err, vec, pushal и лишь ПОТОМ ds/es/fs/gs — поэтому снизу вверх:
//   GS=0 FS=1 ES=2 DS=3 EDI=4 ESI=5 EBP=6 ESP=7 EBX=8 EDX=9 ECX=10 EAX=11
//   VEC=12 ERR=13 EIP=14 CS=15 ... (ответ пишем в EAX=frame[11]).
extern "C" void syscall_dispatch(unsigned int* regs) {
    unsigned int num = regs[11];    // EAX — номер вызова
    unsigned int a1  = regs[8];     // EBX
    unsigned int a2  = regs[10];    // ECX
    unsigned int a3  = regs[9];     // EDX
    unsigned int a4  = regs[5];     // ESI
    unsigned int a5  = regs[4];     // EDI
    regs[11] = sys_dispatch(num, a1, a2, a3, a4, a5);
}

// --- Ловушки CPU: печатаем диагноз и останавливаемся (не мигающая перезагрузка)
extern "C" void cpu_fault(unsigned int* regs) {
    unsigned int vec = regs[12], err = regs[13], eip = regs[14];
    unsigned int cr2 = 0;
    asm volatile("mov %%cr2, %0" : "=r"(cr2));
    cmd_print("CPU fault: vector ");
    print_dec(vec);
    cmd_print(", error ");
    print_dec(err);
    cmd_print(" at eip=");
    print_dec(eip);
    cmd_print(" addr=");
    print_dec(cr2);
    cmd_print(" cs="); print_dec(regs[15]);
    cmd_print(" ds="); print_dec(regs[3]);
    cmd_print(" es="); print_dec(regs[2]);
    cmd_print(" fs="); print_dec(regs[1]);
    cmd_print(" gs="); print_dec(regs[0]);
    cmd_print(" eax="); print_dec(regs[11]);
    cmd_print(" ebx="); print_dec(regs[8]);
    cmd_print(" uesp="); print_dec(regs[17]);
    cmd_print(" ss="); print_dec(regs[18]);
    cmd_print("\nSystem halted.\n");
    for (;;) asm volatile("hlt");
}
