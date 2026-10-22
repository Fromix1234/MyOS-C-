// ===========================================================================
//  splash — заставка загрузки: логотип, подпись и полоса прогресса (~1.4 с)
// ===========================================================================
//  Заставка рисуется ядром до запуска оболочки. Любая клавиша её пропускает.
//  В COM1 печатается телеметрия (key=..., wait=...ms) — по ней видно, что
//  загрузка не зависла и клавиатура не прислала «залётный» ACK.
// ===========================================================================

use crate::{arch, input, log, video};

pub fn draw_splash() {
    log::puts(b"boot: splash\n");
    video::fill_rect(0, 0, video::width(), video::height(), video::C_BLACK);

    let scale = 5;
    let logo = b"MyOS";
    let lw = 4 * 8 * scale;
    let lx = (video::width() - lw) / 2;
    let ly = video::height() / 2 - 100;
    video::draw_text_big(lx, ly, logo, scale, video::C_WHITE);

    video::fill_rect(lx, ly + 16 * scale + 8, lw, 3, video::C_WHITE);

    let sub = b"mini operating system";
    video::draw_text((video::width() - 21 * 8) / 2, ly + 16 * scale + 24, sub, video::C_WHITE);

    // Полоса загрузки и подпись под ней.
    let bw = 320;
    let bh = 14;
    let bx = (video::width() - bw) / 2;
    let by = ly + 16 * scale + 64;
    video::draw_rect(bx, by, bw, bh, video::C_WHITE);
    video::draw_text(
        (video::width() - 18 * 8) / 2,
        by + 28,
        b"Loading kernel...",
        video::C_WHITE,
    );

    let t0 = arch::millis();
    let mut shown: i32 = -1;
    let mut sc: i32 = -1;
    let mut dt_end: u32 = 0;

    loop {
        let dt = arch::millis().wrapping_sub(t0);
        if dt >= 1400 {
            dt_end = dt;
            break;
        }
        let w = (dt as u64 * (bw as u64 - 6) / 1400) as i32;
        if w != shown {
            shown = w;
            video::fill_rect(bx + 3, by + 3, bw - 6, bh - 6, video::C_BLACK);
            video::fill_rect(bx + 3, by + 3, w, bh - 6, video::C_WHITE);
        }
        let k = input::poll_key();
        if k >= 0 {
            sc = k;
            dt_end = arch::millis().wrapping_sub(t0);
            break;
        }
        unsafe { arch::pause() };
    }

    log::puts(b"splash-exit: key=");
    if sc < 0 {
        log::puts(b"none");
    } else {
        log::print_hex8(sc as u8);
    }
    log::puts(b" wait=");
    log::print_dec(dt_end);
    log::puts(b"ms\n");

    // Очистка перед рабочим столом.
    video::fill_rect(0, 0, video::width(), video::height(), video::C_BLACK);
}

/// Экран фатальной ошибки загрузки (как boot_error_screen в C++-ядре).
pub fn boot_error_screen(msg: &[u8]) -> ! {
    video::fill_rect(0, 0, video::width(), video::height(), video::C_BLACK);
    video::draw_text(40, 40, b"MyOS - boot error", video::C_RED);
    video::draw_text(40, 72, msg, video::C_WHITE);
    log::puts(b"boot: fatal: ");
    log::puts(msg);
    log::puts(b"\n");
    loop {
        unsafe { arch::hlt() }
    }
}
