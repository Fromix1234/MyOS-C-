// ===========================================================================
//  log — системный лог (dmesg): кольцевой буфер в ОЗУ + вывод в COM1
// ===========================================================================
//  Всё, что ядро печатает, попадает и в буфер (его читает оболочка через
//  SYS_LOG_READ), и в COM1 (по нему тесты видят загрузку). Как в Linux:
//  пользователь видит dmesg, а отладка идёт по serial-порту.
// ===========================================================================

use crate::arch;

const LOG_SIZE: usize = 16384; // степень двойки: индекс = голова & маска

static mut LOG_BUF: [u8; LOG_SIZE] = [0; LOG_SIZE];
static mut LOG_HEAD: u32 = 0; // сколько байт всего записано
static mut LOG_TAIL: u32 = 0; // сколько байт всего прочитано

pub fn reset() {
    unsafe {
        LOG_HEAD = 0;
        LOG_TAIL = 0;
    }
}

pub fn putc(c: u8) {
    unsafe {
        let h = LOG_HEAD;
        LOG_BUF[(h as usize) & (LOG_SIZE - 1)] = c;
        LOG_HEAD = h.wrapping_add(1);
        if LOG_HEAD.wrapping_sub(LOG_TAIL) as usize > LOG_SIZE {
            LOG_TAIL = LOG_HEAD - LOG_SIZE as u32;
        }
    }
    arch::serial_putc(c);
}

pub fn puts(s: &[u8]) {
    for &b in s {
        putc(b);
    }
}

/// Прочитать до `max` байт лога в буфер ядра. Возврат: сколько прочитано.
pub fn read(dst: *mut u8, max: u32) -> u32 {
    unsafe {
        let avail = LOG_HEAD.wrapping_sub(LOG_TAIL);
        let n = if avail < max { avail } else { max };
        for i in 0..n {
            let b = LOG_BUF[((LOG_TAIL + i) as usize) & (LOG_SIZE - 1)];
            dst.add(i as usize).write(b);
        }
        LOG_TAIL = LOG_TAIL.wrapping_add(n);
        n
    }
}

pub fn print_dec(mut v: u32) {
    if v == 0 {
        putc(b'0');
        return;
    }
    let mut tmp = [0u8; 10];
    let mut i = 0;
    while v > 0 {
        tmp[i] = b'0' + (v % 10) as u8;
        v /= 10;
        i += 1;
    }
    while i > 0 {
        i -= 1;
        putc(tmp[i]);
    }
}

pub fn print_hex8(v: u8) {
    const HEX: &[u8; 16] = b"0123456789abcdef";
    putc(HEX[(v >> 4) as usize]);
    putc(HEX[(v & 0x0F) as usize]);
}

/// Печать строки, завершённой нулём (указатель из ядра, не из ring 3).
pub unsafe fn print_cstr(mut p: *const u8) {
    while *p != 0 {
        putc(*p);
        p = p.add(1);
    }
}
