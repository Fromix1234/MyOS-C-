// ===========================================================================
//  KERNEL k7-time: системный таймер (PIT 100 Гц, IRQ0) и часы CMOS
// ===========================================================================
//  В v1.2 время считалось «лачением» счётчика PIT без прерываний. В v2.0
//  таймер — настоящий источник тиков: IRQ0 приходит 100 раз в секунду,
//  увеличивает счётчик, и по нему же работает планировщик (см. k7b/k8).
//  millis() теперь просто «тики × 10» — без обращения к железу.
// ===========================================================================

volatile unsigned int timer_ticks = 0;       // тики IRQ0 с момента pit_init
int timer_slice = 0;                         // счётчик для кванта планировщика

// Обработчик тика (зовёт irq_dispatch из k7b)
void timer_tick() {
    timer_ticks++;
    timer_slice++;
}

// Миллисекунды с запуска (100 Гц -> 10 мс на тик)
unsigned int millis() { return timer_ticks * 10; }

// Запустить канал 0 PIT: режим 3 (меандр), 100 Гц
void pit_init() {
    unsigned int div = 1193182 / 100;        // ≈11932 — 100 прерываний в секунду
    outb(0x43, 0x36);                        // канал 0, младший/старший, режим 3
    outb(0x40, (unsigned char)(div & 0xFF));
    outb(0x40, (unsigned char)((div >> 8) & 0xFF));
    timer_ticks = 0;
    timer_slice = 0;
}

// --- Часы реального времени CMOS (порты 0x70/0x71), как в v1.2 -------------
unsigned char cmos_read(unsigned char reg) {
    outb(0x70, reg);                         // выбрать регистр
    return inb(0x71);                        // прочитать значение
}
int bcd2bin(unsigned char b) { return (b & 0x0F) + ((b >> 4) * 10); }

// Секунды с начала суток — для часов оболочки и SYS_TIME
unsigned int day_secs() {
    return (unsigned int)(bcd2bin(cmos_read(4)) * 3600 +
                          bcd2bin(cmos_read(2)) * 60 +
                          bcd2bin(cmos_read(0)));
}
