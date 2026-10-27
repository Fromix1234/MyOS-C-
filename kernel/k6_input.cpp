// ===========================================================================
//  KERNEL k6: PS/2 — клавиатура и мышь (опрос заменён прерываниями IRQ1/IRQ12)
// ===========================================================================
//  В v1.2 главный цикл сам «крутился» вокруг порта 0x64 и вычитывал мышь
//  и клавиатуру. В v2.0 так нельзя: оболочка — отдельная программа (ring 3),
//  а ядро должно собирать ввод ФОНОМ, как настоящий драйвер:
//    * IRQ1  (вектор 33) — вычитать сканкод клавиши и положить в очередь;
//    * IRQ12 (вектор 44) — собрать 3‑байтный пакет мыши в событие движения;
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

// State for mouse packet processing
volatile unsigned char ms_cycle = 0;
volatile unsigned char ms_b[4]; // support up to 4‑byte packets (IntelliMouse)
volatile bool mouse_extended = false; // true when 4‑byte packets are active
volatile unsigned int mouse_packet_len = 3; // 3 or 4 depending on mode
volatile int mouse_wheel = 0; // accumulated scroll wheel delta

void mouse_reset() {
    ms_h = 0; ms_t = 0; ms_cycle = 0; mouse_extended = false; mouse_packet_len = 3; mouse_wheel = 0;
}

// Forward declaration: the command helper is defined below.
int mouse_cmd(unsigned char v);

// Helper: send the "Set Sample Rate" command and wait for ACK
static unsigned char mouse_set_sample_rate(unsigned char rate) {
    int ack = mouse_cmd(0xF3);
    if (ack != 0xFA) return (unsigned char)ack; // not ACK, abort
    return (unsigned char)mouse_cmd(rate);
}

// Read one byte from the auxiliary port with a short timeout (-1 = none).
static int mouse_read_byte(void) {
    for (int i = 0; i < 100000; i++) {
        unsigned char st = inb(0x64);
        if (st & 0x01) {
            unsigned char d = inb(0x60);
            if (st & 0x20) return (int)d;   // bit 5 = data came from the mouse
        }
    }
    return -1;
}

// Try to enable IntelliMouse (4‑byte packets) using the standard 200‑100‑80 sequence.
// Returns true if the mouse reports an extended ID (0x03 or 0x04).
static bool enable_intel_mouse(void) {
    // Flush stale keyboard/ACK bytes before the magic sequence.
    for (int i = 0; i < 32 && (inb(0x64) & 0x01); i++) inb(0x60);
    // Set sample rates 200, 100, 80
    if (mouse_set_sample_rate(200) != 0xFA) return false;
    if (mouse_set_sample_rate(100) != 0xFA) return false;
    if (mouse_set_sample_rate(80)  != 0xFA) return false;
    // Request ID: first ACK, then the actual device id.
    if (mouse_cmd(0xF2) != 0xFA) return false;
    int id = mouse_read_byte();
    // IDs 0x03 (IntelliMouse) and 0x04 (IntelliMouse with scroll wheel) indicate 4‑byte packets
    if (id == 0x03 || id == 0x04) {
        mouse_extended = true;
        mouse_packet_len = 4;
        return true;
    }
    mouse_extended = false;
    mouse_packet_len = 3;
    return false;
}

// IRQ12: mouse sends packets. The first byte must have bit 3 set. ACK/SELF‑TEST
// bytes also have bit 3 set but have overflow bits (6‑7) set, which we ignore
// at the start of a packet.
void mouse_irq() {
    unsigned char b = inb(0x60);
    // A packet always starts with bit 3 set. 0xFA/0xAA are stray ACK/self-test
    // bytes that also match the bit pattern; drop them before syncing.
    if (ms_cycle == 0) {
        if ((b & 0x08) == 0) return;
        if (b == 0xFA || b == 0xAA) return;
    }
    ms_b[ms_cycle++] = b;
    if (ms_cycle < mouse_packet_len) return;
    // Complete packet received
    ms_cycle = 0;
    int dx = (int)ms_b[1];
    int dy = (int)ms_b[2];
    // Handle overflow bits (bits 6/7). If overflow, clamp to +/-255.
    if (ms_b[0] & 0x40) dx = (ms_b[0] & 0x10) ? -255 : 255;
    if (ms_b[0] & 0x80) dy = (ms_b[0] & 0x20) ? -255 : 255;
    // Sign extension for 9‑bit movement
    if (ms_b[0] & 0x10) dx -= 256;
    if (ms_b[0] & 0x20) dy -= 256;
    // Wheel handling for 4‑byte packets (signed 8‑bit delta)
    if (mouse_extended && mouse_packet_len == 4) {
        // Byte 3 is the scroll wheel; treat it as signed
        int wheel = (signed char)ms_b[3];
        mouse_wheel += wheel;
    }
    unsigned int h = ms_h;
    if (h - ms_t < MSQ_SIZE) {
        ms_q[h & (MSQ_SIZE - 1)].dx  = dx;
        ms_q[h & (MSQ_SIZE - 1)].dy  = -dy; // PS/2: Y up, screen down
        ms_q[h & (MSQ_SIZE - 1)].btn = ms_b[0] & 0x07;
        ms_h = h + 1;
    }
}

// Read one mouse event; returns false if queue is empty.
bool mouse_poll(int* dx, int* dy, int* btn) {
    if (ms_t == ms_h) return false;
    *dx  = ms_q[ms_t & (MSQ_SIZE - 1)].dx;
    *dy  = ms_q[ms_t & (MSQ_SIZE - 1)].dy;
    *btn = ms_q[ms_t & (MSQ_SIZE - 1)].btn;
    ms_t++;
    return true;
}

// Retrieve and clear accumulated scroll wheel delta.
int mouse_get_wheel(void) {
    int w = mouse_wheel;
    mouse_wheel = 0;
    return w;
}

// --- PS/2 controller initialization (no polling) --------------------------
void ps2_wr_wait() { for (int i = 0; i < 100000; i++) if (!(inb(0x64) & 0x02)) return; }
void ps2_rd_wait() { for (int i = 0; i < 100000; i++) if (inb(0x64) & 0x01) return; }

// Send a mouse command: prefix 0xD4 (aux device) then data byte.
// Returns the mouse's response (0xFA = ACK on success).
int mouse_cmd(unsigned char v) {
    ps2_wr_wait(); outb(0x64, 0xD4);
    ps2_wr_wait(); outb(0x60, v);
    ps2_rd_wait(); return inb(0x60);
}

int mouse_ack = -1; // response to 0xF4 (diagnostics in UI)

void ps2_init() {
    kbd_reset();
    mouse_reset();
    ps2_wr_wait(); outb(0x64, 0xA8);         // enable auxiliary device (mouse)
    ps2_wr_wait(); outb(0x64, 0x20);         // read controller config byte
    ps2_rd_wait(); unsigned char cfg = inb(0x60);
    cfg = (unsigned char)(cfg & ~0x20);      // enable mouse clock
    cfg = (unsigned char)(cfg | 0x01);       // enable IRQ1 (keyboard)
    cfg = (unsigned char)(cfg | 0x02);       // enable IRQ12 (mouse)
    cfg = (unsigned char)(cfg | 0x40);       // translate scancodes (BIOS style)
    ps2_wr_wait(); outb(0x64, 0x60);
    ps2_wr_wait(); outb(0x60, cfg);
    mouse_cmd(0xF6);                         // set defaults
    mouse_ack = mouse_cmd(0xF4);             // enable data reporting
    // Attempt to enable IntelliMouse 4‑byte packets
    enable_intel_mouse();
    // Flush any pending data
    for (int i = 0; i < 32 && (inb(0x64) & 0x01); i++) inb(0x60);
}
