// ===========================================================================
//  SHELL s8: MyC code editor + mini compiler
// ===========================================================================
//  MyOS v2.0 module. Split mechanically from kernel.cpp v1.2
//  (git a81d4f4) and then reworked: see README section 11.

// ===========================================================================
//  РЕДАКТОР КОДА MyC — из дока (Code) или командой code
// ===========================================================================
//  Сверху — редактор C-подобного кода. Снизу — панель «куда что легло»:
//  адреса буферов в ОЗУ, файл на диске (MyFS), байты машинного кода и
//  регистры CPU после запуска. F5 — скомпилировать в настоящий x86-код,
//  F6 — запустить его. Язык MyC: int, присваивания, + - * /, скобки, return.

bool code_active = false;
int  code_part = 1;                     // раздел MyFS для сохранения/открытия

char edit_buf[2048];                    // исходник (строки через '\n')
int  edit_len = 0;
int  edit_cur = 0;                      // позиция курсора в буфере
int  edit_top = 0;                      // первая видимая строка
char code_msg[64];                      // сообщение внизу окна
int  code_in = 0;                       // 0 нет, 1 сохранение, 2 открытие
char cin_buf[32]; int cin_len = 0;      // имя файла в диалоге
int  cdx = 0, cdy = 0;                  // где нарисован курсор (для мигания)
unsigned int code_blink = 0;            // последняя «половина секунды» мигания

// куда сохранили / откуда открыли (для строки «диск» в панели)
bool code_disk_ok = false;
int  code_disk_part = 0;
unsigned int code_disk_lba = 0, code_disk_sec = 0, code_disk_size = 0;
char code_disk_name[24];

void code_msg_set(const char* s) {
    int i = 0; while (s[i] && i < 63) { code_msg[i] = s[i]; i++; } code_msg[i] = 0;
}

// --- Разбор буфера на строки (пересчитывается при отрисовке) ---
int lstart[160]; int lcount = 0;
void code_lines() {
    lcount = 0; lstart[0] = 0; lcount = 1;
    for (int i = 0; i < edit_len && lcount < 159; i++)
        if (edit_buf[i] == '\n') lstart[lcount++] = i + 1;
}
void code_cursor_line(int* L, int* C) {
    int l = 0;
    for (int i = 0; i < lcount; i++) { if (lstart[i] <= edit_cur) l = i; else break; }
    *L = l; *C = edit_cur - lstart[l];
}

// --- Правка текста ---
void edit_insert(char c) {
    if (edit_len >= 2047) return;
    for (int i = edit_len; i > edit_cur; i--) edit_buf[i] = edit_buf[i - 1];
    edit_buf[edit_cur++] = c;
    edit_len++;
}
void edit_backspace() {
    if (edit_cur <= 0) return;
    for (int i = edit_cur - 1; i < edit_len - 1; i++) edit_buf[i] = edit_buf[i + 1];
    edit_cur--; edit_len--;
}
void code_move_v(int dir) {             // курсор на строку вверх/вниз (столбец сохраняем)
    code_lines();
    int L, C; code_cursor_line(&L, &C);
    int nL = L + dir;
    if (nL < 0 || nL >= lcount) return;
    int s = lstart[nL];
    int e = (nL + 1 < lcount) ? lstart[nL + 1] - 1 : edit_len;
    int len = e - s;
    edit_cur = s + ((C < len) ? C : len);
}
void code_home() { code_lines(); int L, C; code_cursor_line(&L, &C); edit_cur -= C; }
void code_end() {
    code_lines(); int L, C; code_cursor_line(&L, &C);
    edit_cur = (L + 1 < lcount) ? lstart[L + 1] - 1 : edit_len;
}

// ===========================================================================
//  МИНИ-КОМПИЛЯТОР: MyC -> настоящие байты x86
// ===========================================================================
//  Значения переменных лежат в mcv_value (массив ОЗУ), машинный код — в code_buf.
//  Компилятор кладёт код в code_buf абсолютными адресами, потом ядро ВЫЗЫВАЕТ
//  этот буфер как функцию (call) — код исполняется по-настоящему.
unsigned char code_buf[1536];
int  code_len = 0;
int  mcv_count = 0;                     // сколько переменных объявлено
char mcv_name[8][16];                   // их имена (для показа)
int  mcv_value[8];                      // их значения — вот эти ячейки ОЗУ видит код
unsigned int mc_regs[7];                // eax, ebx, ecx, edx, esp, ebp, eflags
int  mc_retval = 0;                     // значение из return
bool mc_ok = false;                     // последняя компиляция успешна
bool mc_ran = false;                    // программа уже запускалась
bool mc_bad = false;                    // ошибка компиляции
char mc_err[40]; int mc_err_line = 0;   // текст и строка ошибки
int  mc_line = 1;                       // текущая строка при разборе
const char* mc_src;                     // указатель разбора по исходнику
int  mc_guard[8];                       // счётчики итераций циклов (защита от вечного цикла)
int  mc_loop_depth = 0;                 // текущая вложенность циклов
int  mc_ret_jumps[16]; int mc_ret_count = 0;   // откуда прыгать в эпилог (все return)

// токен: 0 конец, 1 число, 2 имя, 3 символ
// для символов: mt_sym (первый) + mt_sym2 (второй, для ==, !=, <=, >=)
int mt_type = 0; unsigned int mt_num = 0; char mt_name[16];
char mt_sym = 0; char mt_sym2 = 0;

// проверки токена: «это одиночный символ c?» / «это пара c1c2?»
int mt_is(char c)  { return mt_type == 3 && mt_sym == c && mt_sym2 == 0; }
int mt_is2(char a, char b) { return mt_type == 3 && mt_sym == a && mt_sym2 == b; }

void mc_fail(const char* msg) {
    if (mc_bad) return;
    mc_bad = true;
    int i = 0; while (msg[i] && i < 39) { mc_err[i] = msg[i]; i++; } mc_err[i] = 0;
    mc_err_line = mc_line;
}
int mc_ident_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

// Следующий токен (пробелы и переводы строк пропускаем)
void mc_next() {
    for (;;) {
        while (*mc_src == ' ' || *mc_src == '\t' || *mc_src == '\r') mc_src++;
        char c = *mc_src;
        if (c == '\n') { mc_line++; mc_src++; continue; }
        if (c == 0) { mt_type = 0; return; }
        if (c >= '0' && c <= '9') {
            unsigned int v = 0;
            while (*mc_src >= '0' && *mc_src <= '9') { v = v * 10 + (unsigned)(*mc_src - '0'); mc_src++; }
            mt_type = 1; mt_num = v; return;
        }
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_') {
            int i = 0;
            while (mc_ident_char(*mc_src) && i < 15) mt_name[i++] = *mc_src++;
            mt_name[i] = 0; mt_type = 2; return;
        }
        if (c == '+' || c == '-' || c == '*' || c == '/' ||
            c == '(' || c == ')' || c == ';' || c == '{' || c == '}') {
            mt_sym = c; mt_sym2 = 0; mc_src++; mt_type = 3; return;
        }
        if (c == '=' || c == '!' || c == '<' || c == '>') {   // может быть 2 символа
            mt_sym = c; mt_sym2 = 0;
            if (mc_src[1] == '=') { mt_sym2 = '='; mc_src += 2; }  // == != <= >=
            else mc_src++;
            mt_type = 3; return;
        }
        mc_src++;                                 // чужой символ — пропускаем
    }
}

int mcv_find(const char* n) {
    for (int i = 0; i < mcv_count; i++) if (str_eq(mcv_name[i], n)) return i;
    return -1;
}

// --- Генератор байтов ---
void em1(unsigned char b) {
    if (code_len >= (int)sizeof(code_buf)) { mc_fail("program too long"); return; }
    code_buf[code_len++] = b;
}
void em4(unsigned int v) { em1((unsigned char)v); em1((unsigned char)(v >> 8)); em1((unsigned char)(v >> 16)); em1((unsigned char)(v >> 24)); }
void em_mov_eax_imm(unsigned int v)  { em1(0xB8); em4(v); }               // mov eax, imm32
void em_load_eax(unsigned int a)     { em1(0xA1); em4(a); }               // mov eax, [a]
void em_store_eax(unsigned int a)    { em1(0xA3); em4(a); }               // mov [a], eax
void em_store_reg(unsigned int a, int reg) {                              // mov [a], <reg>
    if (reg == 0) { em1(0xA3); em4(a); return; }
    em1(0x89); em1((unsigned char)(0x05 | (reg << 3))); em4(a);
}
void em_push_eax()     { em1(0x50); }
void em_pop_eax()      { em1(0x58); }
void em_mov_ecx_eax()  { em1(0x89); em1(0xC1); }                          // mov ecx, eax
void em_add()          { em1(0x01); em1(0xC8); }                          // add eax, ecx
void em_sub()          { em1(0x29); em1(0xC8); }                          // sub eax, ecx
void em_imul()         { em1(0x0F); em1(0xAF); em1(0xC1); }               // imul eax, ecx
void em_div_guard()    { em1(0x85); em1(0xC9); em1(0x74); em1(0x03); }    // test ecx,ecx; jz +3
void em_cdq_idiv()     { em1(0x99); em1(0xF7); em1(0xF9); }               // cdq; idiv ecx
void em_zero_dword(unsigned int a) { em1(0xC7); em1(0x05); em4(a); em4(0); }
void em_ret()          { em1(0xC3); }
// --- Операции для условий и переходов ---
void em_test_eax()     { em1(0x85); em1(0xC0); }              // test eax, eax
void em_cmp_eax_ecx()  { em1(0x39); em1(0xC8); }              // cmp eax, ecx
void em_movzx_eax_al() { em1(0x0F); em1(0xB6); em1(0xC0); }   // movzx eax, al
void em_setcc(int op) {                                       // результат сравнения -> 0/1
    em1(0x0F);
    if (op == 1) em1(0x9C);                                   // setl  <
    else if (op == 2) em1(0x9F);                              // setg  >
    else if (op == 3) em1(0x9E);                              // setle <=
    else if (op == 4) em1(0x9D);                              // setge >=
    else if (op == 5) em1(0x94);                              // sete  ==
    else em1(0x95);                                           // setne !=
    em1(0xC0);
    em_movzx_eax_al();
}
// Прыжки: сначала пишем нулевое смещение, потом em_patch впишет настоящее
void em_jmp(int* p)  { em1(0xE9); *p = code_len; em4(0); }    // jmp rel32
void em_jz(int* p)   { em1(0x0F); em1(0x84); *p = code_len; em4(0); }  // jz (rax==0)
void em_jae(int* p)  { em1(0x0F); em1(0x83); *p = code_len; em4(0); }  // jae (>=, беззнак.)
void em_patch(int p, int target) {                            // вписать rel32 = target-(p+4)
    unsigned int rel = (unsigned int)(target - (p + 4));
    if (p >= 0 && p + 3 < (int)sizeof(code_buf)) {
        code_buf[p]     = (unsigned char)rel;
        code_buf[p + 1] = (unsigned char)(rel >> 8);
        code_buf[p + 2] = (unsigned char)(rel >> 16);
        code_buf[p + 3] = (unsigned char)(rel >> 24);
    }
}
void em_inc_mem(unsigned int a) { em1(0xFF); em1(0x05); em4(a); }               // inc dword [a]
void em_cmp_mem_imm(unsigned int a, unsigned int v) { em1(0x81); em1(0x3D); em4(a); em4(v); }

// --- Разбор выражений ---
//  expr = cmp;  cmp = addsub [ relop addsub ];  addsub = term{+-term};
//  term = factor{*/factor};  factor = число | имя | ( expr )
//  relop = < | > | <= | >= | == | !=     (результат сравнения — 0 или 1)
void mc_expr();
void mc_factor() {
    if (mc_bad) return;
    if (mt_type == 1) { em_mov_eax_imm(mt_num); mc_next(); return; }
    if (mt_type == 2) {
        int i = mcv_find(mt_name);
        if (i < 0) { mc_fail("unknown variable"); return; }
        em_load_eax((unsigned int)(unsigned long)&mcv_value[i]);
        mc_next(); return;
    }
    if (mt_is('(')) {
        mc_next(); mc_expr();
        if (mt_is(')')) mc_next();
        else mc_fail("expected )");
        return;
    }
    mc_fail("expected value");
}
void mc_term() {
    mc_factor();
    for (;;) {
        if (mc_bad) return;
        if (!(mt_is('*') || mt_is('/'))) return;
        char op = mt_sym; mc_next();
        em_push_eax();                            // левый операнд — в стек
        mc_factor();
        if (mc_bad) return;
        em_mov_ecx_eax();                         // правый операнд — в ecx
        em_pop_eax();                             // левый — обратно в eax
        if (op == '*') em_imul();
        else { em_div_guard(); em_cdq_idiv(); }   // деление с защитой от нуля
    }
}
void mc_addsub() {
    mc_term();
    for (;;) {
        if (mc_bad) return;
        if (!(mt_is('+') || mt_is('-'))) return;
        char op = mt_sym; mc_next();
        em_push_eax();
        mc_term();
        if (mc_bad) return;
        em_mov_ecx_eax();
        em_pop_eax();
        if (op == '+') em_add(); else em_sub();
    }
}
void mc_cmp() {                                   // addsub [ relop addsub ]
    mc_addsub();
    if (mc_bad) return;
    int op = 0;                                   // 1< 2> 3<= 4>= 5== 6!=
    if      (mt_is('<'))          op = 1;
    else if (mt_is('>'))          op = 2;
    else if (mt_is2('<', '='))    op = 3;
    else if (mt_is2('>', '='))    op = 4;
    else if (mt_is2('=', '='))    op = 5;
    else if (mt_is2('!', '='))    op = 6;
    if (!op) return;
    mc_next();
    em_push_eax();
    mc_addsub();
    if (mc_bad) return;
    em_mov_ecx_eax();                             // ecx = правый
    em_pop_eax();                                 // eax = левый
    em_cmp_eax_ecx();                             // флаги по (eax - ecx)
    em_setcc(op);                                 // eax = 0 или 1
}
void mc_expr() { mc_cmp(); }

// --- Разбор операторов ---
void mc_stmt();                                   // объявлен заранее (нужен для блока)

// Один оператор или блок в фигурных скобках { ... }
void mc_block_or_stmt() {
    if (mt_is('{')) {
        mc_next();
        while (!mc_bad && !mt_is('}') && mt_type != 0) mc_stmt();
        if (!mt_is('}')) { mc_fail("expected }"); return; }
        mc_next();
        return;
    }
    mc_stmt();
}

// if (условие) оператор [else оператор]
void mc_if() {
    if (!mt_is('(')) { mc_fail("expected ("); return; }
    mc_next(); mc_expr();
    if (mc_bad) return;
    if (!mt_is(')')) { mc_fail("expected )"); return; }
    mc_next();
    em_test_eax();
    int jfalse; em_jz(&jfalse);                   // условие == 0 -> на else/конец
    mc_block_or_stmt();
    if (mc_bad) return;
    if (mt_type == 2 && str_eq(mt_name, "else")) {
        mc_next();
        int jend; em_jmp(&jend);
        em_patch(jfalse, code_len);               // сюда — если условие ложно
        mc_block_or_stmt();
        if (mc_bad) return;
        em_patch(jend, code_len);                 // сюда — после ветки then
    } else {
        em_patch(jfalse, code_len);
    }
}

// while (условие) оператор — со счётчиком итераций (защита от вечного цикла)
void mc_while() {
    if (!mt_is('(')) { mc_fail("expected ("); return; }
    mc_next();
    if (mc_loop_depth >= 8) { mc_fail("loops too deep"); return; }
    int slot = mc_loop_depth++;                   // у каждого цикла свой счётчик
    em_zero_dword((unsigned int)(unsigned long)&mc_guard[slot]);
    int top = code_len;                           // начало цикла: отсюда и считаем условие
    mc_expr();                                    // условие ВНУТРИ цикла (должно пересчитываться!)
    if (mc_bad) return;
    if (!mt_is(')')) { mc_fail("expected )"); return; }
    mc_next();
    em_test_eax();
    int jout; em_jz(&jout);                       // условие == 0 -> выход
    mc_block_or_stmt();
    if (mc_bad) return;
    em_inc_mem((unsigned int)(unsigned long)&mc_guard[slot]);
    em_cmp_mem_imm((unsigned int)(unsigned long)&mc_guard[slot], 20000000u);
    int jguard; em_jae(&jguard);                  // слишком много итераций -> выход
    int jback; em_jmp(&jback);
    em_patch(jback, top);                         // назад к проверке условия
    em_patch(jout, code_len);                     // обычный выход
    em_patch(jguard, code_len);                   // выход по защите
    mc_loop_depth--;
}

void mc_stmt() {
    if (mc_bad) return;
    if (mt_is(';')) { mc_next(); return; }        // пустой оператор
    if (mt_is('{')) { mc_block_or_stmt(); return; }
    if (mt_type == 2 && str_eq(mt_name, "if"))    { mc_next(); mc_if();    return; }
    if (mt_type == 2 && str_eq(mt_name, "while")) { mc_next(); mc_while(); return; }
    if (mt_type == 2 && str_eq(mt_name, "return")) {
        mc_next(); mc_expr();
        if (mc_bad) return;
        em_store_eax((unsigned int)(unsigned long)&mc_retval);
        if (mt_is(';')) mc_next(); else { mc_fail("expected ;"); return; }
        if (mc_ret_count < 16) {                  // прыжок в эпилог (а не «сквозь» код)
            int p; em_jmp(&p); mc_ret_jumps[mc_ret_count++] = p;
        }
        return;
    }
    if (mt_type == 2 && str_eq(mt_name, "int")) {
        mc_next();
        if (mt_type != 2) { mc_fail("expected name"); return; }
        if (mcv_count >= 8) { mc_fail("too many variables"); return; }
        int idx = mcv_count;
        for (int i = 0; i < 16; i++) mcv_name[idx][i] = 0;
        int i = 0; while (mt_name[i] && i < 15) { mcv_name[idx][i] = mt_name[i]; i++; }
        mcv_count++;
        mc_next();
        if (mt_is('=')) {
            mc_next(); mc_expr();
            if (mc_bad) return;
            em_store_eax((unsigned int)(unsigned long)&mcv_value[idx]);
        } else {
            em_zero_dword((unsigned int)(unsigned long)&mcv_value[idx]);
        }
        if (mt_is(';')) mc_next(); else { mc_fail("expected ;"); return; }
        return;
    }
    if (mt_type == 2) {                            // присваивание: name = expr;
        int idx = mcv_find(mt_name);
        if (idx < 0) { mc_fail("unknown variable"); return; }
        mc_next();
        if (!mt_is('=')) { mc_fail("expected ="); return; }
        mc_next(); mc_expr();
        if (mc_bad) return;
        em_store_eax((unsigned int)(unsigned long)&mcv_value[idx]);
        if (mt_is(';')) mc_next(); else { mc_fail("expected ;"); return; }
        return;
    }
    mc_fail("expected statement");
}

// Скомпилировать edit_buf -> code_buf
void mc_compile() {
    code_len = 0; mcv_count = 0; mc_bad = false; mc_ok = false; mc_ran = false;
    mc_ret_count = 0; mc_loop_depth = 0;
    mc_line = 1; mc_err[0] = 0; mc_err_line = 0;
    edit_buf[edit_len] = 0;                        // исходник как C-строка
    mc_src = edit_buf;
    mc_next();
    while (!mc_bad && mt_type != 0) mc_stmt();
    if (mc_bad) return;
    // ЭПИЛОГ: сюда ведут все return; снять регистры в ОЗУ и вернуться в ядро
    int ep = code_len;
    for (int i = 0; i < mc_ret_count; i++) em_patch(mc_ret_jumps[i], ep);
    em_store_reg((unsigned int)(unsigned long)&mc_regs[0], 0);   // eax
    em_store_reg((unsigned int)(unsigned long)&mc_regs[1], 3);   // ebx
    em_store_reg((unsigned int)(unsigned long)&mc_regs[2], 1);   // ecx
    em_store_reg((unsigned int)(unsigned long)&mc_regs[3], 2);   // edx
    em_store_reg((unsigned int)(unsigned long)&mc_regs[4], 4);   // esp
    em_store_reg((unsigned int)(unsigned long)&mc_regs[5], 5);   // ebp
    em1(0x9C); em1(0x58);                                        // pushfd; pop eax
    em_store_eax((unsigned int)(unsigned long)&mc_regs[6]);      // eflags
    em_ret();
    mc_ok = true;
}

// Запустить скомпилированный код
void mc_run() {
    for (int i = 0; i < 8; i++) { mcv_value[i] = 0; mc_guard[i] = 0; }
    for (int i = 0; i < 7; i++) mc_regs[i] = 0;
    mc_retval = 0; mc_ran = false;
    if (!mc_ok) return;
    void (*fn)() = (void(*)())code_buf;            // указатель на наши байты
    fn();                                          // ВЫПОЛНЯЕМ сгенерированный код
    mc_ran = true;
}

// ===========================================================================
//  ОТРИСОВКА окна редактора
// ===========================================================================
int cww, cwh, cwx, cwy;

// Курсор: белый блок (или стереть блок и вернуть символ)
void code_draw_cursor(bool on) {
    char ch = 0;
    if (edit_cur < edit_len && edit_buf[edit_cur] != '\n') ch = edit_buf[edit_cur];
    if (ch < 32) ch = ch ? ' ' : 0;
    if (on) {
        fill_rect(cdx, cdy, 8, 16, C_WHITE);
        if (ch) draw_char(cdx, cdy, ch, C_BLACK);
    } else {
        fill_rect(cdx, cdy, 8, 16, C_BLACK);
        if (ch) draw_char(cdx, cdy, ch, TEXT_COL);
    }
}

// Мигание курсора (вызывается из главного цикла в простое)
void code_tick() {
    if (!code_active || code_in != 0) return;
    unsigned int t = millis();
    if (t / 500 == code_blink) return;
    code_blink = t / 500;
    code_draw_cursor((code_blink & 1) == 0);
}

// Панель «куда что легло»: адреса, диск, байты, регистры
void code_panel(int ytop, int ybot) {
    int tx = cwx + 10;
    int cols = (cww - 20) / 8;
    char b[200]; char row[200]; int k;
    int y = ytop;

    k = sk_str(b, 0, "CODE 0x"); k = sk_hex32(b, k, (unsigned int)(unsigned long)code_buf);
    k = sk_str(b, k, " "); k = sk_num(b, k, (unsigned int)code_len); k = sk_str(b, k, "B");
    k = sk_str(b, k, "    VARS 0x"); k = sk_hex32(b, k, (unsigned int)(unsigned long)mcv_value);
    k = sk_str(b, k, " x"); k = sk_num(b, k, (unsigned int)mcv_count);
    b[k] = 0; draw_text(tx, y, b, TEXT_COL); y += 14;

    k = sk_str(b, 0, "SRC  0x"); k = sk_hex32(b, k, (unsigned int)(unsigned long)edit_buf);
    k = sk_str(b, k, " "); k = sk_num(b, k, (unsigned int)edit_len); k = sk_str(b, k, "B");
    k = sk_str(b, k, "    REGS 0x"); k = sk_hex32(b, k, (unsigned int)(unsigned long)mc_regs);
    b[k] = 0; draw_text(tx, y, b, TEXT_COL); y += 14;

    k = 0;
    if (code_disk_ok) {
        k = sk_str(b, k, "DISK P"); k = sk_num(b, k, (unsigned int)code_disk_part);
        k = sk_str(b, k, " "); 
        for (int i = 0; i < 24 && code_disk_name[i]; i++) b[k++] = code_disk_name[i];
        k = sk_str(b, k, " @ LBA "); k = sk_num(b, k, code_disk_lba);
        k = sk_str(b, k, " x"); k = sk_num(b, k, code_disk_sec); k = sk_str(b, k, " sect");
    } else {
        k = sk_str(b, k, "DISK not saved yet");
    }
    b[k] = 0; draw_text(tx, y, b, TEXT_COL); y += 14;

    if (!mc_ok) {
        if (mc_bad) {
            k = sk_str(b, 0, "COMPILE ERROR line "); k = sk_num(b, k, (unsigned int)mc_err_line);
            k = sk_str(b, k, ": "); k = sk_str(b, k, mc_err);
        } else {
            k = sk_str(b, 0, "Not compiled yet - press F5");
        }
        b[k] = 0; draw_text(tx, y, b, TEXT_COL); y += 14;
    } else {
        int show = code_len; if (show > 60) show = 60;
        k = sk_str(b, 0, "BIN ");
        for (int i = 0; i < show && k < 190; i++) { k = sk_hex8(b, k, code_buf[i]); b[k++] = ' '; }
        if (show < code_len) k = sk_str(b, k, "...");
        b[k] = 0;
        int n = 0; while (b[n]) n++;
        for (int off = 0; off < n && y <= ybot; off += cols) {
            int m = 0;
            for (int i = off; i < n && i < off + cols; i++) row[m++] = b[i];
            row[m] = 0;
            draw_text(tx, y, row, TEXT_COL);
            y += 14;
        }
    }

    if (mc_ran) {
        k = sk_str(b, 0, "EAX "); k = sk_hex32(b, k, mc_regs[0]);
        k = sk_str(b, k, "  ECX "); k = sk_hex32(b, k, mc_regs[2]);
        k = sk_str(b, k, "  ESP "); k = sk_hex32(b, k, mc_regs[4]);
        b[k] = 0; draw_text(tx, y, b, TEXT_COL); y += 14;
        k = sk_str(b, 0, "EBX "); k = sk_hex32(b, k, mc_regs[1]);
        k = sk_str(b, k, "  EDX "); k = sk_hex32(b, k, mc_regs[3]);
        k = sk_str(b, k, "  EBP "); k = sk_hex32(b, k, mc_regs[5]);
        b[k] = 0; draw_text(tx, y, b, TEXT_COL); y += 14;
        k = sk_str(b, 0, "EFLAGS "); k = sk_hex32(b, k, mc_regs[6]);
        k = sk_str(b, k, "   RESULT = ");
        int rv = mc_retval;
        if (rv < 0) { b[k++] = '-'; rv = -rv; }
        k = sk_num(b, k, (unsigned int)rv);
        b[k] = 0; draw_text(tx, y, b, TEXT_COL); y += 14;
    } else if (mc_ok) {
        k = sk_str(b, 0, "Registers: press F6 to run and capture them");
        b[k] = 0; draw_text(tx, y, b, TEXT_COL); y += 14;
    }
}

void draw_code() {
    cww = win_w(680); cwh = win_h(500);
    win_wp[APP_CODE] = cww; win_hp[APP_CODE] = cwh;
    cwx = win_x[APP_CODE]; cwy = win_y[APP_CODE];
    char t[80]; int k = sk_str(t, 0, "Code - MyC  [P");
    k = sk_num(t, k, (unsigned int)code_part); k = sk_str(t, k, "]"); t[k] = 0;
    draw_window_frame(cwx, cwy, cww, cwh, t);

    int ed_y = cwy + TITLE_H + 6;
    int ed_rows = (cwh - TITLE_H - 60 - 130) / 16;
    if (ed_rows < 6) ed_rows = 6;
    if (ed_rows > 20) ed_rows = 20;
    int ed_h = ed_rows * 16;
    int pan_y = ed_y + ed_h + 10;

    code_lines();
    int gutter = 4 * 8;
    int cols = (cww - 20 - gutter) / 8;
    int cl, cc; code_cursor_line(&cl, &cc);
    if (cl < edit_top) edit_top = cl;
    if (cl >= edit_top + ed_rows) edit_top = cl - ed_rows + 1;
    if (edit_top < 0) edit_top = 0;

    code_blink = millis() / 500;                   // синхронизируем мигание
    cdx = -1000; cdy = -1000;
    for (int r = 0; r < ed_rows; r++) {
        int li = edit_top + r;
        if (li >= lcount) continue;
        int y = ed_y + r * 16;
        char nb[8]; int nk = sk_num(nb, 0, (unsigned int)(li + 1));    // номер строки
        nb[nk] = 0;                                                    // sk_num не закрывает строку
        draw_text(cwx + 8 + (3 - nk) * 8, y, nb, TEXT_COL);
        int s = lstart[li];
        int e = (li + 1 < lcount) ? lstart[li + 1] - 1 : edit_len;
        int col = 0;
        for (int i = s; i < e && col < cols; i++, col++) {
            char chh = edit_buf[i]; if (chh < 32) chh = ' ';
            draw_char(cwx + 8 + gutter + col * 8, y, chh, TEXT_COL);
        }
        if (li == cl && cc < cols) { cdx = cwx + 8 + gutter + cc * 8; cdy = y; }
    }
    if (cdx > -1000) code_draw_cursor(true);

    draw_line(cwx + 8, pan_y - 6, cwx + cww - 9, pan_y - 6, TEXT_COL);
    code_panel(pan_y, cwy + cwh - 24);

    // нижняя строка: подсказка, диалог или сообщение
    int by = cwy + cwh - 20;
    if (code_in == 1) {
        k = sk_str(t, 0, "Save as: ");
        for (int i = 0; i < cin_len; i++) t[k++] = cin_buf[i];
        t[k++] = '_'; t[k] = 0;
    } else if (code_in == 2) {
        k = sk_str(t, 0, "Open: ");
        for (int i = 0; i < cin_len; i++) t[k++] = cin_buf[i];
        t[k++] = '_'; t[k] = 0;
    } else if (code_msg[0]) {
        k = sk_str(t, 0, code_msg); t[k] = 0;
    } else {
        k = sk_str(t, 0, "F2 save  F4 open  F5 compile  F6 run  F7 part  Esc close");
        t[k] = 0;
    }
    draw_text(cwx + 10, by, t, TEXT_COL);
}

// ===========================================================================
//  КЛАВИШИ редактора кода
// ===========================================================================
void code_input_done() {
    cin_buf[cin_len] = 0;
    if (cin_len == 0) { code_in = 0; draw_code(); return; }
    if (code_in == 1) {                             // СОХРАНИТЬ в MyFS
        edit_buf[edit_len] = 0;
        int r = fs_write(code_part, cin_buf, edit_buf, true);
        if (r == 0) {
            int n = fs_load_table(code_part);
            for (int i = 0; i < n; i++)
                if (str_eq(file_list[i].name, cin_buf)) {
                    code_disk_ok = true; code_disk_part = code_part;
                    code_disk_lba = file_list[i].start; code_disk_size = file_list[i].size;
                    code_disk_sec = (file_list[i].size + 511) / 512;
                    for (int j = 0; j < 24; j++) code_disk_name[j] = 0;
                    int j = 0; while (cin_buf[j] && j < 23) { code_disk_name[j] = cin_buf[j]; j++; }
                    break;
                }
            code_msg_set("Saved to MyFS");
        } else if (r == -2) code_msg_set("Not MyFS - format the partition (Files: F)");
        else if (r == -1) code_msg_set("No such partition");
        else if (r == -3) code_msg_set("File exists");
        else if (r == -4) code_msg_set("File table full");
        else code_msg_set("No space on partition");
    } else {                                        // ОТКРЫТЬ из MyFS
        int n = fs_load_table(code_part);
        if (n == -1) code_msg_set("No such partition");
        else if (n == -2) code_msg_set("Not MyFS (format it first)");
        else {
            int ix = -1;
            for (int i = 0; i < n; i++) if (str_eq(file_list[i].name, cin_buf)) ix = i;
            if (ix < 0) code_msg_set("No such file");
            else {
                int len = fs_read_content(ix, edit_buf, 2047);
                edit_len = len; edit_cur = 0; edit_top = 0;
                mc_ok = false; mc_ran = false; mc_bad = false;
                code_disk_ok = true; code_disk_part = code_part;
                code_disk_lba = file_list[ix].start; code_disk_size = file_list[ix].size;
                code_disk_sec = (file_list[ix].size + 511) / 512;
                for (int j = 0; j < 24; j++) code_disk_name[j] = 0;
                int j = 0; while (cin_buf[j] && j < 23) { code_disk_name[j] = cin_buf[j]; j++; }
                code_msg_set("Opened from MyFS");
            }
        }
    }
    code_in = 0;
    draw_code();
}

void code_input_key(int sc) {
    if (sc == 0x01) { code_in = 0; draw_code(); return; }              // Esc — отмена
    if (sc == 0x0E) { if (cin_len > 0) { cin_len--; draw_code(); } return; }  // Backspace
    if (sc == 0x1C) { code_input_done(); return; }                     // Enter — готово
    if (sc == 0x2A || sc == 0x36) { shift_down = true; return; }       // Shift
    if (sc >= 0x3A) return;                                            // не символ
    char c = (char)sc_ascii[sc];
    if (!c) return;
    if (shift_down) c = apply_shift(c);
    if (cin_len < 24) { cin_buf[cin_len++] = c; draw_code(); }
}

void code_handle_key(int sc) {
    if (sc == 0xE0) { e0_prefix = true; return; }
    bool ext = e0_prefix; e0_prefix = false;
    if (sc & 0x80) {                                  // отпускание Shift — снять флаг
        if ((sc & 0x7F) == 0x2A || (sc & 0x7F) == 0x36) shift_down = false;
        return;
    }
    if (code_msg[0]) { code_msg[0] = 0; }             // сообщение снимаем, клавиша работает
    if (code_in != 0) { code_input_key(sc); return; }

    if (ext) {                                        // стрелки и Home/End
        if (sc == 0x4B) { if (edit_cur > 0) edit_cur--; draw_code(); }            // Left
        if (sc == 0x4D) { if (edit_cur < edit_len) edit_cur++; draw_code(); }     // Right
        if (sc == 0x48) { code_move_v(-1); draw_code(); }                        // Up
        if (sc == 0x50) { code_move_v(1); draw_code(); }                         // Down
        if (sc == 0x47) { code_home(); draw_code(); }                            // Home
        if (sc == 0x4F) { code_end(); draw_code(); }                             // End
        return;
    }
    if (sc == 0x01) { code_active = false; win_close_app(APP_CODE); draw_desktop(); return; }             // Esc — закрыть
    if (sc == 0x3C) { code_in = 1; cin_len = 0; draw_code(); return; }           // F2 — сохранить
    if (sc == 0x3E) { code_in = 2; cin_len = 0; draw_code(); return; }           // F4 — открыть
    if (sc == 0x3F) {                                                            // F5 — компилировать
        mc_compile();
        if (mc_ok) code_msg_set("Compiled - code bytes are ready (F6 to run)");
        else code_msg_set("Compile error - see the panel");
        draw_code();
        return;
    }
    if (sc == 0x40) {                                                            // F6 — запустить
        if (!mc_ok) code_msg_set("Compile first (F5)");
        else { mc_run(); code_msg_set("Program executed - see the registers"); }
        draw_code();
        return;
    }
    if (sc == 0x41) { code_part++; if (code_part > 4) code_part = 1; draw_code(); return; }  // F7 — раздел
    if (sc == 0x1C) {                                                            // Enter — новая строка
        edit_insert('\n');
        draw_code();
        return;
    }
    if (sc == 0x0E) { edit_backspace(); draw_code(); return; }                   // Backspace
    if (sc == 0x2A || sc == 0x36) { shift_down = true; return; }                 // Shift
    if (sc >= 0x3A) return;                                                      // не символ
    char c = (char)sc_ascii[sc];
    if (!c) return;
    if (shift_down) c = apply_shift(c);
    edit_insert(c);
    draw_code();
}

// Открыть редактор (из дока или команды code); при первом открытии — пример
void code_open() {
    win_place(APP_CODE, win_w(680), win_h(500));
    win_raise(APP_CODE);
    code_active = true;
    if (edit_len == 0) {
        // Пример: арифметика + цикл while. Допиши "return a;" и нажми F5/F6.
        const char* demo =
            "int a = 5;\n"
            "int b = 3;\n"
            "a = a * b + 2;\n"
            "int i = 0;\n"
            "while (i < a) {\n"
            "  i = i + 1;\n"
            "}\n";
        int i = 0; while (demo[i]) { edit_buf[i] = demo[i]; i++; }
        edit_len = i;
    }
    edit_cur = edit_len;
    edit_top = 0;
    code_in = 0; code_msg[0] = 0;
    draw_code();
}
