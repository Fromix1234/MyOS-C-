// ===========================================================================
//  sim_main.cpp — ХОСТОВЫЙ СИМУЛЯТОР ОБОЛОЧКИ MyOS (для разработки UI)
// ===========================================================================
//  Зачем: собирать и загружать MyOS в VirtualBox ради каждой правки пикселя
//  долго. Этот файл даёт «фальшивое ядро»: настоящий код оболочки (shell/*.cpp)
//  компилируется как обычная программа под macOS/Linux, рисует в обычную
//  память вместо видеопамяти, а кадр сохраняется в BMP.
//
//  Собирается через tools/sim/build.sh. Ничего из этого файла не попадает в
//  образ ОС: build.ps1 его не видит, и он не включён в unity-сборку shell.cpp.
//
//  Использование:
//      tools/sim/build.sh                       # собрать
//      tools/sim/build.sh --shot desktop.bmp    # собрать и отрисовать кадр
//      tools/sim/sim 1440x900 desktop.bmp       # размер окна + файл кадра
//      tools/sim/sim 1024x768 out.bmp "open:0"  # открыть Terminal
//      formats: open:<dock index> | key:<scancode> | move:<x>,<y> | click
// ===========================================================================

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <string>

#include "../../common/myos.h"

// --- Глобалы оболочки (определены в shell/*.cpp) ---------------------------
extern int W, H, FB_PITCH, FB_BPP, FB_RPOS, FB_GPOS, FB_BPOS;
extern unsigned char* fb;

void init_video();
void init_layout();
void init_desktop_layout();
void draw_desktop();
void cursor_show();
void cursor_hide();
void dock_launch(int i);
void kbd_q_push(int sc);
void mouse_handle();

extern int mouse_x, mouse_y, mouse_ok, mouse_ack, mouse_btn;
extern bool cmd_active, about_active, files_active, set_active;
extern bool tasks_active, game_active, code_active;
extern int win_z[], win_focus, win_ztop;
extern int boot_secs;
extern unsigned int g_kernel_sectors;
extern int demo_pid;

// --- «Ядро»: буфер кадра и ответы на системные вызовы ----------------------
static std::vector<unsigned char> g_fb;
static int g_w = 1024, g_h = 768;
static unsigned int g_time = 10 * 3600 + 39 * 60 + 7;   // 10:39:07 в меню-баре
static int g_ticks = 0;

static std::vector<std::string> g_log;                  // «dmesg» для консоли
static int g_log_pos = 0;

// Заготовка списка файлов MyFS — чтобы окно Files не пустовало.
static void fill_fake_files(myos_file_t* out, int max) {
    static const char* names[] = { "shell.bin", "demo.bin", "notes.txt",
                                   "readme.md", "hello.c", "myc.c" };
    static const unsigned int sizes[] = { 65536, 4096, 220, 1180, 640, 900 };
    int n = (int)(sizeof(names) / sizeof(names[0]));
    if (n > max) n = max;
    for (int i = 0; i < n; i++) {
        int k = 0;
        while (names[i][k] && k < 55) { out[i].name[k] = names[i][k]; k++; }
        out[i].name[k] = 0;
        out[i].size = sizes[i];
        out[i].start = 4096 + (unsigned)i * 512;
    }
}

extern "C" long host_syscall(int num, long a1, long a2, long a3, long a4, long a5) {
    switch (num) {
    case SYS_FB: {                         // отдать описание «видеопамяти»
        myos_fb_t* f = (myos_fb_t*)a1;
        f->addr = (unsigned int)(size_t)g_fb.data();
        f->w = g_w; f->h = g_h; f->bpp = 32; f->pitch = g_w * 4;
        f->rpos = 16; f->gpos = 8; f->bpos = 0;
        return 0;
    }
    case SYS_TIME:  g_time++;  return (long)g_time;
    case SYS_TICKS: g_ticks += 10; return g_ticks;
    case SYS_PID:   return 1;
    case SYS_PRINT: return 0;
    case SYS_LOG_READ: {                   // отдать следующую строку «лога»
        if (g_log_pos >= (int)g_log.size()) return 0;
        const std::string& s = g_log[g_log_pos++];
        int max = (int)a2;
        int n = (int)s.size() + 1;         // как ядро: строка + '\n'
        if (n > max) n = max;
        char* dst = (char*)a1;
        for (int i = 0; i < n - 1; i++) dst[i] = s[i];
        dst[n - 1] = '\n';
        return n;
    }
    case SYS_FS_LIST: {
        int max = (int)a3;
        myos_file_t* out = (myos_file_t*)a2;
        fill_fake_files(out, max);
        return 6;
    }
    case SYS_FS_READ:
    case SYS_FS_READAT: {
        const char* text = "MyOS host simulator: contents of a fake file.\n";
        int max = (int)((num == SYS_FS_READ) ? a4 : a4);
        int n = (int)strlen(text);
        if (n > max) n = max;
        char* dst = (char*)((num == SYS_FS_READ) ? a3 : a3);
        for (int i = 0; i < n; i++) dst[i] = text[i];
        return n;
    }
    case SYS_PART_LIST: {
        myos_part_t* p = (myos_part_t*)a1;
        for (int i = 0; i < 4; i++) { p[i].lba = 0; p[i].sectors = 0; p[i].flags = 0; }
        p[0].lba = 2048; p[0].sectors = 61440; p[0].flags = 1 | 2 | 8;
        return 1;
    }
    case SYS_PART_MK:  { myos_mkpart_t* m = (myos_mkpart_t*)a2; if (m) m->code = -4; return -4; }
    case SYS_INSTALL_PLAN: { myos_install_t* m = (myos_install_t*)a2; if (m) m->code = 0; return 0; }
    case SYS_BOOTPART: {
        myos_boot_t* b = (myos_boot_t*)a1;
        b->index = 1; b->lba = 2048;
        return 1;
    }
    case SYS_INFO: return (a1 == 0) ? 42 : 192;
    case SYS_ALIVE: return 0;
    case SYS_INPUT: return 0;              // ввод подаём прямо в очередь клавиш
    case SYS_DISK_WTEST: return -1;
    default: return 0;
    }
}

// ---------------------------------------------------------------------------
static void write_bmp(const char* path) {
    FILE* f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "cannot write %s\n", path); exit(1); }
    int row = g_w * 3;
    int pad = (4 - (row % 4)) % 4;
    int data = (row + pad) * g_h;
    unsigned char hdr[54] = { 'B', 'M' };
    auto put32 = [&](int off, unsigned int v) {
        hdr[off] = v & 0xFF; hdr[off + 1] = (v >> 8) & 0xFF;
        hdr[off + 2] = (v >> 16) & 0xFF; hdr[off + 3] = (v >> 24) & 0xFF;
    };
    put32(2, 54 + data); put32(10, 54); put32(14, 40);
    put32(18, g_w); put32(22, g_h); put32(26, 1); put32(28, 24);
    put32(34, data);
    fwrite(hdr, 1, 54, f);
    std::vector<unsigned char> line(row + pad, 0);
    for (int y = g_h - 1; y >= 0; y--) {          // BMP пишется снизу вверх
        const unsigned char* src = g_fb.data() + (size_t)y * g_w * 4;
        for (int x = 0; x < g_w; x++) {           // fb: B,G,R,X -> BMP: B,G,R
            line[x * 3 + 0] = src[x * 4 + 0];
            line[x * 3 + 1] = src[x * 4 + 1];
            line[x * 3 + 2] = src[x * 4 + 2];
        }
        fwrite(line.data(), 1, line.size(), f);
    }
    fclose(f);
}

int main(int argc, char** argv) {
    // argv[1] = "WxH", argv[2] = путь к BMP, дальше — сценарий
    if (argc > 1) {
        int w = 0, h = 0;
        if (sscanf(argv[1], "%dx%d", &w, &h) == 2 && w > 320 && h > 200) { g_w = w; g_h = h; }
    }
    const char* out = (argc > 2) ? argv[2] : "desktop.bmp";

    g_fb.assign((size_t)g_w * g_h * 4, 0);
    g_log.push_back("MyOS kernel 3.1-pre booting...");
    g_log.push_back("VESA 1024x768x32 OK, LFB mapped.");
    g_log.push_back("MyFS found on P1 (lba=2048).");
    g_log.push_back("Shell loaded at 0x00400000.");

    // --- тот же порядок, что в shell_main (без главного цикла) --------------
    init_video();
    // В ABI myos_fb_t адрес 32-битный (identity-отображение в ring 3), а на
    // 64-битном хосте буфер лежит выше 4 ГБ. Поэтому после init_video
    // подставляем настоящий указатель на хостовый буфер кадра.
    fb = g_fb.data();
    init_layout();
    init_desktop_layout();
    mouse_ok = 1; mouse_ack = 0xFA;
    mouse_x = W / 2; mouse_y = H / 2;
    cmd_active = about_active = files_active = set_active = false;
    tasks_active = game_active = code_active = false;
    for (int a = 0; a < 8; a++) win_z[a] = 0;
    win_focus = -1; win_ztop = 0;
    boot_secs = (int)g_time;
    g_kernel_sectors = 42;
    demo_pid = -1;

    draw_desktop();
    cursor_show();

    // --- сценарий: open:N | key:0xNN | move:X,Y | click | hide -----------------
    for (int i = 3; i < argc; i++) {
        std::string cmd = argv[i];
        if (cmd == "hide") { cursor_hide(); }
        else if (cmd == "click") {
            mouse_btn = 1; mouse_handle();
            mouse_btn = 0; mouse_handle();
        }
        else if (cmd.rfind("move:", 0) == 0) {
            int x = 0, y = 0;
            sscanf(cmd.c_str() + 5, "%d,%d", &x, &y);
            cursor_hide(); mouse_x = x; mouse_y = y; cursor_show();
        }
        else if (cmd.rfind("key:", 0) == 0) {
            int sc = (int)strtol(cmd.c_str() + 4, nullptr, 0);
            kbd_q_push(sc);
        }
        else if (cmd.rfind("open:", 0) == 0) {
            dock_launch(atoi(cmd.c_str() + 5));
        }
    }
    write_bmp(out);
    printf("%s: %dx%d\n", out, g_w, g_h);
    return 0;
}
