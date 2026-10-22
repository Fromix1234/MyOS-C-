// ===========================================================================
//  video — кадровый буфер VBE: пиксели, прямоугольники, шрифт 8x16
// ===========================================================================
//  Режим ставит stage2 (VESA VBE), а сюда он оставляет «записку» в низкой
//  памяти: адрес, pitch, размеры, bpp и порядок каналов. Ядро только рисует
//  заставку и экран ошибки; весь рабочий стол рисует оболочка в ring 3.
// ===========================================================================

use crate::font::FONT8X16;

// Значения по умолчанию (если записки нет — например, старый загрузчик).
static mut W: i32 = 320;
static mut H: i32 = 200;
static mut FB_PITCH: i32 = 320;
static mut FB_BPP: i32 = 8;
static mut FB_RPOS: i32 = 16;
static mut FB_GPOS: i32 = 8;
static mut FB_BPOS: i32 = 0;
static mut FB: *mut u8 = 0xA0000 as *mut u8;

pub fn width() -> i32 {
    unsafe { W }
}

pub fn height() -> i32 {
    unsafe { H }
}

pub fn fb_addr() -> u32 {
    unsafe { FB as u32 }
}

pub fn bpp() -> i32 {
    unsafe { FB_BPP }
}

pub fn pitch() -> i32 {
    unsafe { FB_PITCH }
}

pub fn rpos() -> i32 {
    unsafe { FB_RPOS }
}

pub fn gpos() -> i32 {
    unsafe { FB_GPOS }
}

pub fn bpos() -> i32 {
    unsafe { FB_BPOS }
}

/// Прочитать «записку» stage2 о видеорежиме VESA VBE и запомнить её.
pub fn init_video() {
    unsafe {
        if core::ptr::read_volatile(0x0510 as *const u16) != 0x4D56 {
            return; // записки нет — остаются значения по умолчанию
        }
        let fba = core::ptr::read_volatile(0x0504 as *const u32);
        let pitch = core::ptr::read_volatile(0x0508 as *const u16) as i32;
        let w = core::ptr::read_volatile(0x050A as *const u16) as i32;
        let h = core::ptr::read_volatile(0x050C as *const u16) as i32;
        let bpp = core::ptr::read_volatile(0x050E as *const u8) as i32;
        let rp = core::ptr::read_volatile(0x0512 as *const u8) as i32;
        let gp = core::ptr::read_volatile(0x0513 as *const u8) as i32;
        let bp = core::ptr::read_volatile(0x0514 as *const u8) as i32;
        if fba != 0 && pitch != 0 && w != 0 && h != 0 && (bpp == 24 || bpp == 32) {
            FB = fba as *mut u8;
            FB_PITCH = pitch;
            FB_BPP = bpp;
            W = w;
            H = h;
            if rp < 32 && gp < 32 && bp < 32 && (rp != gp || gp != bp) {
                FB_RPOS = rp;
                FB_GPOS = gp;
                FB_BPOS = bp;
            }
        }
    }
}

pub const C_BLACK: u32 = 0x000000;
pub const C_WHITE: u32 = 0xFFFFFF;
pub const C_RED: u32 = 0xFF3B30;

fn encode_color(rgb: u32) -> u32 {
    unsafe {
        let r = (rgb >> 16) & 0xFF;
        let g = (rgb >> 8) & 0xFF;
        let b = rgb & 0xFF;
        (r << FB_RPOS) | (g << FB_GPOS) | (b << FB_BPOS)
    }
}

pub fn put_pixel(x: i32, y: i32, rgb: u32) {
    unsafe {
        if x < 0 || y < 0 || x >= W || y >= H {
            return;
        }
        let color = encode_color(rgb);
        let p = FB.add((y * FB_PITCH + x * (FB_BPP / 8)) as usize);
        if FB_BPP == 32 {
            core::ptr::write_volatile(p as *mut u32, color);
        } else if FB_BPP == 24 {
            p.write((color & 0xFF) as u8);
            p.add(1).write(((color >> 8) & 0xFF) as u8);
            p.add(2).write(((color >> 16) & 0xFF) as u8);
        }
    }
}

pub fn fill_rect(x0: i32, y0: i32, w: i32, h: i32, rgb: u32) {
    unsafe {
        let color = encode_color(rgb);
        if FB_BPP == 32 {
            let mut y = 0;
            while y < h {
                let py = y0 + y;
                if py >= 0 && py < H {
                    let row = FB.add((py * FB_PITCH) as usize) as *mut u32;
                    let mut x = 0;
                    while x < w {
                        let px = x0 + x;
                        if px >= 0 && px < W {
                            core::ptr::write_volatile(row.add(px as usize), color);
                        }
                        x += 1;
                    }
                }
                y += 1;
            }
        } else if FB_BPP == 24 {
            let mut y = 0;
            while y < h {
                let py = y0 + y;
                if py >= 0 && py < H {
                    let row = FB.add((py * FB_PITCH) as usize);
                    let mut x = 0;
                    while x < w {
                        let px = x0 + x;
                        if px >= 0 && px < W {
                            let p = row.add((px * 3) as usize);
                            core::ptr::write_volatile(p, (color & 0xFF) as u8);
                            core::ptr::write_volatile(p.add(1), ((color >> 8) & 0xFF) as u8);
                            core::ptr::write_volatile(p.add(2), ((color >> 16) & 0xFF) as u8);
                        }
                        x += 1;
                    }
                }
                y += 1;
            }
        } else {
            // незаданный режим — просто ничего не рисуем
        }
    }
}

pub fn draw_rect(x: i32, y: i32, w: i32, h: i32, rgb: u32) {
    fill_rect(x, y, w, 1, rgb);
    fill_rect(x, y + h - 1, w, 1, rgb);
    fill_rect(x, y, 1, h, rgb);
    fill_rect(x + w - 1, y, 1, h, rgb);
}

pub fn draw_char(x: i32, y: i32, c: u8, rgb: u32) {
    if !(32..=126).contains(&c) {
        return;
    }
    let glyph = &FONT8X16[(c - 32) as usize];
    for row in 0..16 {
        let bits = glyph[row];
        for col in 0..8 {
            if bits & (0x80 >> col) != 0 {
                put_pixel(x + col, y + row as i32, rgb);
            }
        }
    }
}

pub fn draw_text(x: i32, y: i32, s: &[u8], rgb: u32) {
    let mut cx = x;
    for &c in s {
        draw_char(cx, y, c, rgb);
        cx += 8;
    }
}

pub fn draw_char_big(x: i32, y: i32, c: u8, scale: i32, rgb: u32) {
    if !(32..=126).contains(&c) {
        return;
    }
    let glyph = &FONT8X16[(c - 32) as usize];
    for row in 0..16 {
        let bits = glyph[row];
        for col in 0..8 {
            if bits & (0x80 >> col) != 0 {
                fill_rect(x + col as i32 * scale, y + row as i32 * scale, scale, scale, rgb);
            }
        }
    }
}

pub fn draw_text_big(x: i32, y: i32, s: &[u8], scale: i32, rgb: u32) {
    let mut cx = x;
    for &c in s {
        draw_char_big(cx, y, c, scale, rgb);
        cx += 8 * scale;
    }
}
