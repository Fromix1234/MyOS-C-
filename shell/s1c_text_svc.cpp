// ===========================================================================
//  SHELL s1c: «ТЕКСТОВАЯ СЛУЖБА» — очередь клавиатуры и общие строковые хелперы
// ===========================================================================
//  Раньше очередь клавиатуры жила рядом с обёртками сисколлов (s0b), а
//  строковые хелперы (sk_str/sk_num/sk_hex*) — в хвосте того же файла. После
//  разделения s0b (сисколлы / текст / память) они переехали сюда: s0b остался
//  «голым железом» — только int 0x80 и memcpy/memset.
// ===========================================================================

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

// --- Сборка строк: sk_str/sk_num/sk_hex8/sk_hex32 ---------------------------
// Оконный код сначала собирает текст в буфер, потом рисует его на экране.
// Размер копилки: значения почти всегда «маленькие», а на 800x600+ памяти
// у оболочки с запасом — поэтому точность не режем.
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
