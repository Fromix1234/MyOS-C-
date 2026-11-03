// ===========================================================================
//  SHELL s0b: мост в ядро — обёртки системных вызовов (int 0x80)
// ===========================================================================
//  Соглашение (common/myos.h): eax = номер, ebx/ecx/edx/esi/edi = аргументы,
//  ответ приходит в eax. Ядро возвращается через iretd и восстанавливает
//  ВСЕ регистры — «портится» только eax, поэтому операнд "=a".
// ===========================================================================

// --- ХОСТОВЫЙ СИМУЛЯТОР (tools/sim): int 0x80 недоступен вне ring 3 --------
//  Компилируется только при MYOS_HOST_SIM. Вместо прерывания — вызов
//  «фальшивого ядра» из tools/sim/sim_main.cpp.
#ifdef MYOS_HOST_SIM
extern "C" long host_syscall(int num, long a1, long a2, long a3, long a4, long a5);
int sys_call0(int num) { return (int)host_syscall(num, 0, 0, 0, 0, 0); }
int sys_call1(int num, long a1) { return (int)host_syscall(num, a1, 0, 0, 0, 0); }
int sys_call2(int num, long a1, long a2) { return (int)host_syscall(num, a1, a2, 0, 0, 0); }
int sys_call3(int num, long a1, long a2, long a3) { return (int)host_syscall(num, a1, a2, a3, 0, 0); }
int sys_call4(int num, long a1, long a2, long a3, long a4) { return (int)host_syscall(num, a1, a2, a3, a4, 0); }
int sys_call5(int num, long a1, long a2, long a3, long a4, long a5) { return (int)host_syscall(num, a1, a2, a3, a4, a5); }
#else
// --- Вызовы с 0..5 аргументами (a1=EBX, a2=ECX, a3=EDX, a4=ESI, a5=EDI) -----
int sys_call0(int num) {
    int r;
    asm volatile("int $0x80" : "=a"(r)
                 : "a"(num), "b"(0), "c"(0), "d"(0), "S"(0), "D"(0) : "memory");
    return r;
}
int sys_call1(int num, int a1) {
    int r;
    asm volatile("int $0x80" : "=a"(r)
                 : "a"(num), "b"(a1), "c"(0), "d"(0), "S"(0), "D"(0) : "memory");
    return r;
}
int sys_call2(int num, int a1, int a2) {
    int r;
    asm volatile("int $0x80" : "=a"(r)
                 : "a"(num), "b"(a1), "c"(a2), "d"(0), "S"(0), "D"(0) : "memory");
    return r;
}
int sys_call3(int num, int a1, int a2, int a3) {
    int r;
    asm volatile("int $0x80" : "=a"(r)
                 : "a"(num), "b"(a1), "c"(a2), "d"(a3), "S"(0), "D"(0) : "memory");
    return r;
}
int sys_call4(int num, int a1, int a2, int a3, int a4) {
    int r;
    asm volatile("int $0x80" : "=a"(r)
                 : "a"(num), "b"(a1), "c"(a2), "d"(a3), "S"(a4), "D"(0) : "memory");
    return r;
}
int sys_call5(int num, int a1, int a2, int a3, int a4, int a5) {
    int r;
    asm volatile("int $0x80" : "=a"(r)
                 : "a"(num), "b"(a1), "c"(a2), "d"(a3), "S"(a4), "D"(a5) : "memory");
    return r;
}
#endif // MYOS_HOST_SIM

// --- Частые обёртки ---------------------------------------------------------
int sys_time()  { return sys_call0(SYS_TIME);  }
int sys_ticks() { return sys_call0(SYS_TICKS); }
void sys_debug(const char* s) { sys_call1(SYS_PRINT, (int)s); }   // маркер в dmesg
int sys_pid()   { return sys_call0(SYS_PID);   }
int sys_info(int what) { return sys_call1(SYS_INFO, what); }
int sys_alive(int pid) { return sys_call1(SYS_ALIVE, pid); }
int sys_spawn(const char* name, unsigned int base) {
    return sys_call2(SYS_SPAWN, (int)name, (int)base);
}
void sys_yield() { sys_call0(SYS_YIELD); }
void sys_get_fb(myos_fb_t* f) { sys_call1(SYS_FB, (int)f); }
int sys_log_read(char* dst, int max) { return sys_call2(SYS_LOG_READ, (int)dst, max); }
