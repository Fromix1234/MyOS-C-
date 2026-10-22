// ===========================================================================
//  input — PS/2: клавиатура (IRQ1) и мышь (IRQ12), очереди событий
// ===========================================================================
//  Драйвер собирает ввод ФОНОМ, как настоящая ОС:
//    * IRQ1  (вектор 33) — сканкод клавиши в очередь;
//    * IRQ12 (вектор 44) — 3-байтный пакет мыши в событие движения;
//    * оболочка забирает всё пачкой через SYS_INPUT, программы — по клавише
//      через SYS_KEY (владелец клавиатуры назначается при SYS_SPAWN).
// ===========================================================================

use crate::arch::{inb, outb};

// --- Очередь сканкодов ------------------------------------------------------
const KBDQ_SIZE: u32 = 256;
static mut KBD_Q: [u8; KBDQ_SIZE as usize] = [0; KBDQ_SIZE as usize];
static mut KBD_H: u32 = 0; // заполняет IRQ1
static mut KBD_T: u32 = 0; // забирает ядро

fn kbd_head() -> u32 {
    unsafe { core::ptr::read_volatile(core::ptr::addr_of!(KBD_H)) }
}
fn kbd_tail() -> u32 {
    unsafe { core::ptr::read_volatile(core::ptr::addr_of!(KBD_T)) }
}
fn set_kbd_head(v: u32) {
    unsafe { core::ptr::write_volatile(core::ptr::addr_of_mut!(KBD_H), v) }
}
fn set_kbd_tail(v: u32) {
    unsafe { core::ptr::write_volatile(core::ptr::addr_of_mut!(KBD_T), v) }
}

pub fn kbd_reset() {
    set_kbd_head(0);
    set_kbd_tail(0);
}

/// IRQ1: байт в 0x60 — сканкод (make/break, как есть).
pub fn kbd_irq() {
    unsafe {
        let sc = inb(0x60);
        let h = kbd_head();
        if h.wrapping_sub(kbd_tail()) < KBDQ_SIZE {
            KBD_Q[(h & (KBDQ_SIZE - 1)) as usize] = sc;
            set_kbd_head(h.wrapping_add(1));
        }
    }
}

/// Прочитать один сканкод; -1 — очередь пуста.
pub fn poll_key() -> i32 {
    let t = kbd_tail();
    if t == kbd_head() {
        return -1;
    }
    let sc = unsafe { KBD_Q[(t & (KBDQ_SIZE - 1)) as usize] as i32 };
    set_kbd_tail(t.wrapping_add(1));
    sc
}

/// Слить очередь (перед заставкой — убрать «залётные» байты PS/2).
pub fn drain_keys() {
    while poll_key() >= 0 {}
}

// --- Очередь событий мыши ---------------------------------------------------
#[derive(Clone, Copy)]
struct MouseEv {
    dx: i32,
    dy: i32,
    btn: i32,
}

const MSQ_SIZE: u32 = 64;
static mut MS_Q: [MouseEv; MSQ_SIZE as usize] = [MouseEv { dx: 0, dy: 0, btn: 0 }; MSQ_SIZE as usize];
static mut MS_H: u32 = 0;
static mut MS_T: u32 = 0;
static mut MS_CYCLE: u8 = 0;
static mut MS_B: [u8; 3] = [0; 3];
pub static mut MOUSE_ACK: i32 = -1; // ответ на 0xF4 (диагностика)

fn ms_head() -> u32 {
    unsafe { core::ptr::read_volatile(core::ptr::addr_of!(MS_H)) }
}
fn ms_tail() -> u32 {
    unsafe { core::ptr::read_volatile(core::ptr::addr_of!(MS_T)) }
}
fn set_ms_head(v: u32) {
    unsafe { core::ptr::write_volatile(core::ptr::addr_of_mut!(MS_H), v) }
}
fn set_ms_tail(v: u32) {
    unsafe { core::ptr::write_volatile(core::ptr::addr_of_mut!(MS_T), v) }
}

pub fn mouse_reset() {
    set_ms_head(0);
    set_ms_tail(0);
    unsafe { MS_CYCLE = 0 };
}

/// IRQ12: мышь присылает 3-байтные пакеты. Первый байт: бит 3 всегда 1,
/// биты 6-7 — переполнение X/Y (в норме 0). ACK (0xFA) и самотест (0xAA)
/// тоже имеют бит 3, но у них взведены биты 6-7 — отсекаем на фазе 0.
pub fn mouse_irq() {
    unsafe {
        let b = inb(0x60);
        if MS_CYCLE == 0 && (b & 0xC8) != 0x08 {
            return; // это не начало пакета
        }
        MS_B[MS_CYCLE as usize] = b;
        MS_CYCLE += 1;
        if MS_CYCLE < 3 {
            return;
        }
        MS_CYCLE = 0;
        let mut dx = MS_B[1] as i32;
        let mut dy = MS_B[2] as i32;
        if MS_B[0] & 0x10 != 0 {
            dx -= 256; // знак X (доп. код)
        }
        if MS_B[0] & 0x20 != 0 {
            dy -= 256; // знак Y (доп. код)
        }
        let h = ms_head();
        if h.wrapping_sub(ms_tail()) < MSQ_SIZE {
            let e = &mut MS_Q[(h & (MSQ_SIZE - 1)) as usize];
            e.dx = dx;
            e.dy = -dy; // PS/2: Y вверх, экран: вниз
            e.btn = (MS_B[0] & 0x07) as i32;
            set_ms_head(h.wrapping_add(1));
        }
    }
}

/// Прочитать одно событие мыши.
pub fn mouse_poll() -> Option<(i32, i32, i32)> {
    let t = ms_tail();
    if t == ms_head() {
        return None;
    }
    let e = unsafe { MS_Q[(t & (MSQ_SIZE - 1)) as usize] };
    set_ms_tail(t.wrapping_add(1));
    Some((e.dx, e.dy, e.btn))
}

// --- Инициализация PS/2 -----------------------------------------------------
fn ps2_wr_wait() {
    for _ in 0..100000 {
        if unsafe { inb(0x64) } & 0x02 == 0 {
            return;
        }
    }
}

fn ps2_rd_wait() {
    for _ in 0..100000 {
        if unsafe { inb(0x64) } & 0x01 != 0 {
            return;
        }
    }
}

/// Команда мыши: префикс 0xD4 («вспомогательное устройство»), затем байт.
fn mouse_cmd(v: u8) -> i32 {
    unsafe {
        ps2_wr_wait();
        outb(0x64, 0xD4);
        ps2_wr_wait();
        outb(0x60, v);
        ps2_rd_wait();
        inb(0x60) as i32
    }
}

/// Инициализация контроллера PS/2: клавиатура + мышь, БЕЗ polling.
pub fn ps2_init() {
    kbd_reset();
    mouse_reset();
    unsafe {
        ps2_wr_wait();
        outb(0x64, 0xA8); // включить вспомогательное устройство
        ps2_wr_wait();
        outb(0x64, 0x20); // прочитать байт конфигурации
        ps2_rd_wait();
        let mut cfg = inb(0x60);
        cfg &= !0x20; // открыть тактовый генератор мыши
        cfg |= 0x01; // разрешить IRQ1 (клавиатура)
        cfg |= 0x02; // разрешить IRQ12 (мышь)
        cfg |= 0x40; // трансляция сканкодов (как у BIOS)
        ps2_wr_wait();
        outb(0x64, 0x60);
        ps2_wr_wait();
        outb(0x60, cfg);
        let _ = mouse_cmd(0xF6); // настройки по умолчанию
        MOUSE_ACK = mouse_cmd(0xF4); // включить передачу данных мыши
        let mut i = 0;
        while i < 32 && (inb(0x64) & 0x01) != 0 {
            let _ = inb(0x60);
            i += 1;
        }
    }
}
