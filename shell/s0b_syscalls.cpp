// ===========================================================================
//  SHELL s0b: мост в ядро — обёртки системных вызовов (int 0x80)
// ===========================================================================
//  Соглашение (common/myos.h): eax = номер, ebx/ecx/edx/esi/edi = аргументы,
//  ответ приходит в eax. Ядро возвращается через iretd и восстанавливает
//  ВСЕ регистры — «портится» только eax, поэтому операнд "=a".
// ===========================================================================

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

// --- Клавиатура оболочки: очередь сканкодов из событий ядра -----------------
//  input_pump (s4) раскладывает EV_KEY сюда, а оконный код читает poll_key().
unsigned char kbd_q[256];
int kbd_h = 0, kbd_t = 0;

void kbd_q_push(int sc) {
    if (kbd_h - kbd_t >= 256) return;            // переполнение — теряем новое
    kbd_q[kbd_h & 255] = (unsigned char)sc;
    kbd_h++;
}
int poll_key() {
    if (kbd_t == kbd_h) return -1;               // очередь пуста
    int sc = kbd_q[kbd_t & 255];
    kbd_t++;
    return sc;
}

// --- Память/строки (оболочке тоже не дают libc: -nostdlib) ------------------
// Компилятор вправе звать их сам (копии структур, обнуление буферов).
extern "C" void* memset(void* d, int c, unsigned int n) {
    unsigned char* p = (unsigned char*)d;
    while (n--) *p++ = (unsigned char)c;
    return d;
}
extern "C" void* memcpy(void* d, const void* s, unsigned int n) {
    unsigned char* a = (unsigned char*)d;
    const unsigned char* b = (const unsigned char*)s;
    while (n--) *a++ = *b++;
    return d;
}
extern "C" unsigned int strlen(const char* s) {
    unsigned int n = 0;
    while (s[n]) n++;
    return n;
}
extern "C" int strcmp(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

// --- Сборка строк для надписей: sk_str/sk_num/sk_hex8/sk_hex32 --------------
// Оконный код сначала собирает текст в буфер, потом рисует его на экране.
// (В ядре такие же хелперы; сборки разные — потому копия, а не ссылка.)
int sk_str(char* b, int k, const char* s) { while (*s) b[k++] = *s++; return k; }
int sk_num(char* b, int k, unsigned int v) {
    char t[12]; int i = 0;
    if (v == 0) t[i++] = '0';
    while (v > 0) { t[i++] = (char)('0' + v % 10); v /= 10; }
    while (i > 0) b[k++] = t[--i];
    return k;
}
int sk_hex8(char* b, int k, unsigned char v) {
    const char* h = "0123456789ABCDEF";
    b[k++] = h[(v >> 4) & 0x0F];
    b[k++] = h[v & 0x0F];
    return k;
}
int sk_hex32(char* b, int k, unsigned int v) {
    k = sk_hex8(b, k, (unsigned char)((v >> 24) & 0xFF));
    k = sk_hex8(b, k, (unsigned char)((v >> 16) & 0xFF));
    k = sk_hex8(b, k, (unsigned char)((v >> 8) & 0xFF));
    return sk_hex8(b, k, (unsigned char)(v & 0xFF));
}
