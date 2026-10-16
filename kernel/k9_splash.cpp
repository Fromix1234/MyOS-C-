// ===========================================================================
//  KERNEL k9: boot splash (logo + progress bar)
// ===========================================================================
//  MyOS v2.0 module. Split mechanically from kernel.cpp v1.2
//  (git a81d4f4) and then reworked: see README section 11.

void draw_char_big(int x, int y, char c, int scale, unsigned int color) {
    if (c < 32 || c > 126) return;
    const unsigned char* glyph = font8x16[c - 32];
    for (int row = 0; row < 16; row++) {
        unsigned char bits = glyph[row];
        for (int col = 0; col < 8; col++)
            if (bits & (0x80 >> col))
                fill_rect(x + col * scale, y + row * scale, scale, scale, color);
    }
}
void draw_text_big(int x, int y, const char* s, int scale, unsigned int color) {
    while (*s) { draw_char_big(x, y, *s, scale, color); x += 8 * scale; s++; }
}

// Заставка: крупный логотип «MyOS», подпись и полоса загрузки (~1.4 с).
// Любая клавиша пропускает заставку.
void draw_splash() {
    log_puts("boot: splash\n");         // метка для тестов и dmesg (COM1)
    fill_rect(0, 0, W, H, C_BLACK);
    const int scale = 5;                                 // 40x80 пикселей на символ
    const char* logo = "MyOS";
    int lw = 4 * 8 * scale;
    int lx = (W - lw) / 2, ly = H / 2 - 100;
    draw_text_big(lx, ly, logo, scale, TEXT_COL);        // логотип
    fill_rect(lx, ly + 16 * scale + 8, lw, 3, TEXT_COL); // линия под ним
    const char* sub = "mini operating system";
    draw_text((W - 21 * 8) / 2, ly + 16 * scale + 24, sub, TEXT_COL);
    // полоса загрузки
    int bw = 320, bh = 14;
    int bx = (W - bw) / 2, by = ly + 16 * scale + 64;
    draw_rect(bx, by, bw, bh, TEXT_COL);
    draw_text((W - 18 * 8) / 2, by + 28, "Loading kernel...", TEXT_COL);
    unsigned int t0 = millis();
    int shown = -1;
    int sc = -1;                                     // ВРЕМЕННО: причина выхода (диагностика)
    unsigned int dtEnd = 0;
    for (;;) {
        unsigned int dt = millis() - t0;
        if (dt >= 1400) { dtEnd = dt; break; }
        int w = (int)((unsigned long)dt * (unsigned long)(bw - 6) / 1400);
        if (w != shown) {                                // перерисуем только при изменении
            shown = w;
            fill_rect(bx + 3, by + 3, bw - 6, bh - 6, C_BLACK);
            fill_rect(bx + 3, by + 3, w, bh - 6, TEXT_COL);
        }
        sc = poll_key();
        if (sc >= 0) { dtEnd = millis() - t0; break; }   // клавиша — пропустить заставку
    }
    // Телеметрия загрузки: в COM1/dmesg видно, чем и через сколько вышла заставка
    // («key=none wait=1400ms» — норма; key=FA wait=0ms — залётный ACK, см. k10).
    log_puts("splash-exit: key=");
    if (sc < 0) log_puts("none"); else print_hex8((unsigned char)sc);
    log_puts(" wait="); print_dec(dtEnd); log_puts("ms\n");
    fill_rect(0, 0, W, H, C_BLACK);                      // очистка перед рабочим столом
}
