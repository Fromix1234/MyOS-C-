// ===========================================================================
//  KERNEL k6: PS/2 — клавиатура и мышь (опрос заменён прерываниями IRQ1/IRQ12)
// ===========================================================================
//  В v1.2 главный цикл сам «крутился» вокруг порта 0x64 и вычитывал мышь
//  и клавиатуру. В v2.0 так нельзя: оболочка — отдельная программа (ring 3),
//  а ядро должно собирать ввод ФОНОМ, как настоящий драйвер:
//    * IRQ1  (вектор 33) — вычитать сканкод клавиши и положить в очередь;
//    * IRQ12 (вектор 44) — собрать 3-байтный пакет мыши в событие движения;
//    * оболочка забирает всё пачкой через SYS_INPUT (события), а программы —
//      сканкод за сканкодом через SYS_KEY, как раньше;
//    * владелец ввода (кто получает КЛАВИШИ) назначается при SYS_SPAWN:
//      запущенной программе отдаём клавиатуру, при её выходе (SYS_EXIT)
//      клавиатура возвращается «родителю» (оболочке) — старое поведение
//      команды ring3 сохранено.
// ===========================================================================

// --- Очередь сканкодов (заполняет IRQ1, читает ядро по запросу оболочки) ---
const unsigned int KBDQ_SIZE = 256;
volatile unsigned char kbd_q[KBDQ_SIZE];
volatile unsigned int  kbd_h = 0, kbd_t = 0;

void kbd_reset() { kbd_h = 0; kbd_t = 0; }

// IRQ1: байт в 0x60 — сканкод (make/break, как есть)
void kbd_irq() {
    unsigned char sc = inb(0x60);
    unsigned int h = kbd_h;
    if (h - kbd_t < KBDQ_SIZE) {
        kbd_q[h & (KBDQ_SIZE - 1)] = sc;
        kbd_h = h + 1;
    }
}

// Прочитать один сканкод из очереди; -1 — очередь пуста
int poll_key() {
    if (kbd_t == kbd_h) return -1;
    int sc = kbd_q[kbd_t & (KBDQ_SIZE - 1)];
    kbd_t++;
    return sc;
}

// --- Очередь событий мыши (заполняет IRQ12) --------------------------------
struct mouse_ev_t { int dx, dy, btn; };     // dy уже в ЭКРАННЫХ координатах
const unsigned int MSQ_SIZE = 64;
volatile mouse_ev_t ms_q[MSQ_SIZE];
volatile unsigned int ms_h = 0, ms_t = 0;

unsigned char ms_cycle = 0;
unsigned char ms_b[3];
void mouse_reset() { ms_h = 0; ms_t = 0; ms_cycle = 0; }

// IRQ12: мышь посылает 3-байтные пакеты. Первый байт: бит 3 всегда 1, биты
// 6-7 — переполнение X/Y (в норме 0). Байты ACK (0xFA) и самотеста (0xAA)
// тоже имеют бит 3, но у них взведены биты 6-7 — отсекаем их на фазе 0,
// иначе один такой байт сбивает фазу и курсор прыгает (проверено отладкой).
void mouse_irq() {
    unsigned char b = inb(0x60);
    if (ms_cycle == 0 && (b & 0xC8) != 0x08) return;   // это не начало пакета
    ms_b[ms_cycle++] = b;
    if (ms_cycle < 3) return;
    ms_cycle = 0;
    int dx = (int)ms_b[1];
    int dy = (int)ms_b[2];
    if (ms_b[0] & 0x10) dx -= 256;                     // знак X (доп. код)
    if (ms_b[0] & 0x20) dy -= 256;                     // знак Y (доп. код)
    unsigned int h = ms_h;
    if (h - ms_t < MSQ_SIZE) {
        ms_q[h & (MSQ_SIZE - 1)].dx  = dx;
        ms_q[h & (MSQ_SIZE - 1)].dy  = -dy;            // PS/2: Y вверх, экран: вниз
        ms_q[h & (MSQ_SIZE - 1)].btn = ms_b[0] & 0x07;
        ms_h = h + 1;
    }
}

// Прочитать одно событие мыши; false — ничего нет
bool mouse_poll(int* dx, int* dy, int* btn) {
    if (ms_t == ms_h) return false;
    *dx  = ms_q[ms_t & (MSQ_SIZE - 1)].dx;
    *dy  = ms_q[ms_t & (MSQ_SIZE - 1)].dy;
    *btn = ms_q[ms_t & (MSQ_SIZE - 1)].btn;
    ms_t++;
    return true;
}

// --- Инициализация PS/2: клавиатура + мышь, БЕЗ polling --------------------
// Последовательность из v1.2 (проверена), но теперь мы РАЗРЕШАЕМ IRQ1/IRQ12
// в байте конфигурации контроллера (раньше биты только выключались).
void ps2_wr_wait() { for (int i = 0; i < 100000; i++) if (!(inb(0x64) & 0x02)) return; }
void ps2_rd_wait() { for (int i = 0; i < 100000; i++) if (inb(0x64) & 0x01) return; }

// Команда мыши: префикс 0xD4 («вспомогательное устройство»), затем байт.
// Возврат — ответ мыши (успех: 0xFA, ACK).
int mouse_cmd(unsigned char v) {
    ps2_wr_wait(); outb(0x64, 0xD4);
    ps2_wr_wait(); outb(0x60, v);
    ps2_rd_wait(); return inb(0x60);
}

int mouse_ack = -1;                          // ответ на 0xF4 (диагностика в UI)

void ps2_init() {
    kbd_reset();
    mouse_reset();
    ps2_wr_wait(); outb(0x64, 0xA8);         // включить вспомогательное устройство
    ps2_wr_wait(); outb(0x64, 0x20);         // прочитать байт конфигурации
    ps2_rd_wait(); unsigned char cfg = inb(0x60);
    cfg = (unsigned char)(cfg & ~0x20);      // открыть тактовый генератор мыши
    cfg = (unsigned char)(cfg | 0x01);       // разрешить IRQ1 (клавиатура)
    cfg = (unsigned char)(cfg | 0x02);       // разрешить IRQ12 (мышь)
    cfg = (unsigned char)(cfg | 0x40);       // трансляция сканкодов (как у BIOS)
    ps2_wr_wait(); outb(0x64, 0x60);
    ps2_wr_wait(); outb(0x60, cfg);
    mouse_cmd(0xF6);                         // настройки по умолчанию
    mouse_ack = mouse_cmd(0xF4);             // включить передачу данных мыши
    for (int i = 0; i < 32 && (inb(0x64) & 0x01); i++) inb(0x60);  // слить мусор
}
