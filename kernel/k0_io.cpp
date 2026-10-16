// ===========================================================================
//  KERNEL k0: порты ввода-вывода, COM1 (serial), системный лог (dmesg),
//             печать чисел и мелкие строковые/памятные помощники
// ===========================================================================
//  В MyOS v2.0 (архитектура «как Zen/Linux») у ядра НЕТ своего экрана:
//  всё, что ядро хочет сказать, оно пишет в КОЛЬЦЕВОЙ ЛОГ (dmesg) и в
//  COM1. Оболочка (ring 3) забирает лог системным вызовом SYS_LOG_READ и
//  показывает его в окне консоли — ровно как 'dmesg' в Linux.
//
//  Печать (cmd_print/cmd_putchar/print_dec/...) специально оставлена С ТЕМИ
//  ЖЕ именами, что были в v1.2: тела команд (fs_ls, install, part, ...)
//  переехали сюда БЕЗ изменений — они по-прежнему печатают как раньше,
//  но текст уходит в лог, а не на экран.
// ===========================================================================

// --- Порты ввода-вывода (общение с железом напрямую, только ring 0) --------
void outb(unsigned short port, unsigned char value) {
    asm volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}
unsigned char inb(unsigned short port) {
    unsigned char result;
    asm volatile("inb %1, %0" : "=a"(result) : "Nd"(port));
    return result;
}
unsigned short inw(unsigned short port) {
    unsigned short result;
    asm volatile("inw %1, %0" : "=a"(result) : "Nd"(port));
    return result;
}
void outw(unsigned short port, unsigned short value) {
    asm volatile("outw %0, %1" : : "a"(value), "Nd"(port));
}

// --- COM1: текстовый порт для отладки (работает даже без экрана) ----------
void serial_init() {
    outb(0x3F8 + 1, 0x00);      // выключить прерывания serial
    outb(0x3F8 + 3, 0x80);      // DLAB: настраиваем делитель
    outb(0x3F8 + 0, 0x03);      // 38400 бод (делитель = 3)
    outb(0x3F8 + 1, 0x00);
    outb(0x3F8 + 3, 0x03);      // 8 бит, без чётности, 1 стоп-бит
    outb(0x3F8 + 2, 0xC7);      // FIFO вкл, чистим
    outb(0x3F8 + 4, 0x0B);      // DTR/RTS/OUT2
}
void serial_putc(char c) {
    for (int t = 0; t < 10000; t++)
        if (inb(0x3F8 + 5) & 0x20) break;   // ждём пустой регистр передачи
    outb(0x3F8, (unsigned char)c);
}

// --- Системный лог (кольцевой буфер, «dmesg») ------------------------------
const unsigned int LOG_SIZE = 16384;        // 16 КБ — с запасом на загрузку
char log_buf[LOG_SIZE];
volatile unsigned int log_head = 0;         // куда пишем
volatile unsigned int log_tail = 0;         // откуда читает оболочка

void log_reset() { log_head = 0; log_tail = 0; }

void log_putc(char c) {
    unsigned int h = log_head;
    log_buf[h & (LOG_SIZE - 1)] = c;
    log_head = h + 1;
    if (log_head - log_tail > LOG_SIZE)     // оболочка не успевает — теряем старое
        log_tail = log_head - LOG_SIZE;
    serial_putc(c);                         // и на COM1 (тесты/отладка)
}
void log_puts(const char* s) { while (*s) log_putc(*s++); }

// Прочитать из лога не больше max байт. Возврат: сколько прочитано.
unsigned int log_read(char* dst, unsigned int max) {
    unsigned int n = 0;
    while (n < max && log_tail != log_head) {
        dst[n++] = log_buf[log_tail & (LOG_SIZE - 1)];
        log_tail++;
    }
    return n;
}

// --- Печать в лог (имена из v1.2 — тела команд не менялись) ----------------
void cmd_putchar(char c) { log_putc(c); }
void cmd_print(const char* s) { log_puts(s); }

void print_dec(unsigned int v) {
    char b[12];
    int n = 0;
    if (v == 0) { cmd_putchar('0'); return; }
    while (v) { b[n++] = (char)('0' + v % 10); v /= 10; }
    while (n) cmd_putchar(b[--n]);
}
void print_hex8(unsigned char v) {
    const char* h = "0123456789ABCDEF";
    cmd_putchar(h[v >> 4]);
    cmd_putchar(h[v & 0x0F]);
}
void print2(int n) {                        // ровно две цифры (для часов)
    cmd_putchar((char)('0' + (n / 10) % 10));
    cmd_putchar((char)('0' + n % 10));
}

// --- Памятные/строковые помощники (у ядра нет libc: -nostdlib) -------------
// Компилятор вправе звать эти функции сам (копии структур, обнуление и т.п.),
// поэтому даём им ровно стандартные имена C-символов.
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

// --- Проверка «адрес лежит в памяти программ» (4..8 МБ) --------------------
// Системные вызовы обязаны проверять указатели, как copy_from_user в Linux.
bool user_ok(unsigned int addr, unsigned int len) {
    if (addr < 0x00400000u) return false;             // ниже памяти программ
    if (len > 0x00400000u) return false;
    if (addr + len > 0x00800000u) return false;       // выше памяти программ
    return true;
}

// Скопировать строку из памяти программы в буфер ядра (до max-1 символов).
// Возврат: длина строки, -1 — неверный адрес (программа не может «убежать»).
int copy_str_from_user(char* dst, const char* src, int max) {
    if (!user_ok((unsigned int)src, 1)) return -1;
    int n = 0;
    while (n < max - 1) {
        if (!user_ok((unsigned int)(src + n), 1)) return -1;
        char c = src[n];
        if (!c) break;
        dst[n] = c;
        n++;
    }
    dst[n] = 0;
    return n;
}
