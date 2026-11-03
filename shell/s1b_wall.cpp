// ===========================================================================
//  SHELL s1b: «АКВА-ОБОИ» — панорама гор в туманной дымке (стиль macOS 15)
// ===========================================================================
//  Второй набор обоев рядом с Night Ridge / Bliss Hill. Любая точка экрана
//  считается чистой функцией aqua_pixel(x, y) на целых числах:
//
//    * небо — вертикальный градиент из трёх опорных цветов;
//    * солнце/луна — диск с мягким ореолом (только в горизонте ночной темы);
//    * четыре гряды гор: дальние — светлые и «в дымке», ближние — тёмные;
//      силуэт — сумма двух треугольных волн с разными периодами;
//    * на ближней гряде — рябь снега по хребту.
//
//  Быстрая отрисовка (как у fill_wallpaper): вершины гряд считаются ОДИН РАЗ
//  по колонкам, тела заливаются fill_rect, небо — по строкам.
// ===========================================================================

// Палитра для ТЁМНОЙ темы: ночная панорама в фиолетово-синих тонах.
static const unsigned int AQ_D_SKY0 = 0x05070E;   // зенит
static const unsigned int AQ_D_SKY1 = 0x121A33;   // середина
static const unsigned int AQ_D_SKY2 = 0x33305C;   // у горизонта
static const unsigned int AQ_D_FAR  = 0x2C3050;   // дальняя гряда
static const unsigned int AQ_D_MID  = 0x1F2340;   // средняя
static const unsigned int AQ_D_NEAR = 0x14172A;   // ближняя
static const unsigned int AQ_D_GLOW = 0x6E6AA8;   // ореол луны

// Палитра для СВЕТЛОЙ темы: дневные «долины» — голубое небо, синие гряды.
static const unsigned int AQ_L_SKY0 = 0x2C6FC9;
static const unsigned int AQ_L_SKY1 = 0x6FA9E6;
static const unsigned int AQ_L_SKY2 = 0xD3E7F8;
static const unsigned int AQ_L_FAR  = 0x7FA3CF;
static const unsigned int AQ_L_MID  = 0x4B76B4;
static const unsigned int AQ_L_NEAR = 0x274B84;
static const unsigned int AQ_L_GLOW = 0xFFF3D0;

// Цвет неба на строке y для темы dark (1) / light (0).
unsigned int aqua_sky_color(int y, int dark) {
    int hh = (H > 0 ? H : 768);
    int mid = hh * 52 / 100;
    unsigned int a = dark ? AQ_D_SKY0 : AQ_L_SKY0;
    unsigned int b = dark ? AQ_D_SKY1 : AQ_L_SKY1;
    unsigned int c = dark ? AQ_D_SKY2 : AQ_L_SKY2;
    if (y <= mid) return mix_col(a, b, iclamp(y * 256 / (mid + 1), 0, 256));
    return mix_col(b, c, iclamp((y - mid) * 256 / (hh - mid + 1), 0, 256));
}

// Вершина гряды layer (0 — дальняя .. 2 — ближняя) в колонке x:
// насколько ниже серединной линии «выпирает» гора (0..256).
static int aqua_peak(int x, int layer) {
    int ww = (W > 0 ? W : 1024);
    static const int per[3] = { 4, 7, 11 };          // период крупной волны / W
    static const int per2[3] = { 17, 23, 31 };       // период мелкой / W
    int p1 = ww / per[layer] + 3, p2 = ww / per2[layer] + 2;
    int a = wave_tri(x + layer * 97, p1);
    int b = wave_tri(x * 2 + layer * 31 + 7, p2);
    return (a * 3 + b) / 4;
}

// Строка-«вершина» гряды layer в колонке x (в пикселях).
static int aqua_ridge_y(int x, int layer) {
    int hh = (H > 0 ? H : 768);
    static const int base[3] = { 54, 63, 72 };       // где «подошва», % высоты
    static const int amp[3]  = { 12, 15, 18 };       // высота гор, % высоты
    return hh * base[layer] / 100 - hh * amp[layer] / 100 * aqua_peak(x, layer) / 256;
}

static int aq_top[3][RW_MAXW];
static int aq_usew = 0, aq_useh = 0, aq_usedark = -1;

static void aqua_precompute() {
    aq_usew = (W > RW_MAXW) ? RW_MAXW : W;
    aq_useh = H;
    aq_usedark = ui_dark ? 1 : 0;
    for (int x = 0; x < aq_usew; x++)
        for (int l = 0; l < 3; l++) aq_top[l][x] = aqua_ridge_y(x, l);
}

// Быстрая отрисовка панорамы (композитный путь).
static void fill_wallpaper_aqua_fast() {
    int dark = ui_dark ? 1 : 0;
    if (aq_usew != W || aq_useh != H || aq_usedark != dark) aqua_precompute();

    // 1) небо — строками (над первой грядой, остальное перекроется)
    int skymax = H;
    for (int x = 0; x < W; x++) if (aq_top[0][x] < skymax) skymax = aq_top[0][x];
    if (skymax < 0) skymax = 0;
    for (int y = 0; y < skymax; y++) fill_rect(0, y, W, 1, aqua_sky_color(y, dark));

    // 2) солнце/луна: мягкое свечение у горизонта (только ночью — луна)
    if (dark) {
        int cx = W * 74 / 100, cy = H * 30 / 100, R = H / 42;
        if (R < 8) R = 8;
        for (int dy = -R * 5; dy <= R * 5; dy++) {
            int yy = cy + dy;
            if (yy < 0 || yy >= skymax) continue;
            int k = 256 - iclamp((iabs(dy) * 256) / (R * 5 + 1), 0, 256);
            blend_span(cx - R * 5, yy, R * 10, AQ_D_GLOW, (k * k) >> 10);
        }
        fill_circle_a(cx, cy, R, 0xEDE9F5, 235);
    }

    // 3) гряды: дальняя светлее, ближняя темнее — «воздушная перспектива»
    unsigned int body[3]  = { dark ? AQ_D_FAR : AQ_L_FAR,
                              dark ? AQ_D_MID : AQ_L_MID,
                              dark ? AQ_D_NEAR : AQ_L_NEAR };
    for (int l = 0; l < 3; l++) {
        unsigned int edge = mix_col(body[l], 0xFFFFFF, dark ? 22 : 46);   // хребет
        for (int x = 0; x < aq_usew; x++) {
            int t = aq_top[l][x];
            if (t < 0) t = 0;
            if (t >= H) continue;
            int y0 = t;
            for (int y = t; y < H; y++) {                    // подошва — внизу
                if (y - y0 > 0) break;                       // сама заливка ниже
            }
            fill_rect(x, t, 1, H - t, body[l]);
            put_pixel(x, t, edge);
            if (t + 1 < H) put_pixel(x, t + 1, mix_col(body[l], edge, 128));
        }
    }
}

// Медленный, но точный путь: каждая точка — aqua_pixel (используется там,
// где нужно восстановить маленькую область до обоев).
unsigned int aqua_pixel(int x, int y) {
    int dark = ui_dark ? 1 : 0;
    int l = -1;
    for (int k = 2; k >= 0; k--) if (y >= aqua_ridge_y(x, k)) { l = k; break; }
    if (l < 0) return aqua_sky_color(y, dark);
    static const unsigned int dbody[3] = { AQ_D_FAR, AQ_D_MID, AQ_D_NEAR };
    static const unsigned int lbody[3] = { AQ_L_FAR, AQ_L_MID, AQ_L_NEAR };
    unsigned int b = dark ? dbody[l] : lbody[l];
    int t = aqua_ridge_y(x, l);
    if (y == t) return mix_col(b, 0xFFFFFF, dark ? 34 : 60);
    return b;
}

void fill_wallpaper_aqua() { fill_wallpaper_aqua_fast(); }
