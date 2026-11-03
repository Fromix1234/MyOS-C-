// ===========================================================================
//  kernel.cpp — ЯДРО MyOS v2.0 (сборка «как Zen/Linux»): unity-сборка
// ===========================================================================
//  Ядро в ring 0 держит ТОЛЬКО железо и системные вызовы:
//    k0  — порты, COM1, системный лог (dmesg), печать чисел, mem/str;
//    k1  — видеопамять, примитивы, шрифт (заставка и экран паники);
//    k2  — ATA PIO (диск);
//    k3* — MyFS: структуры, операции с файлами, чтение;
//    k4* — разделы, установка системы, «записка» VBR;
//    k6  — PS/2: клавиатура и мышь (IRQ1/IRQ12, очереди событий);
//    k7  — страницы (PSE 4 МБ), GDT/TSS, шлюзы IDT;
//    k7t — таймер 100 Гц и часы CMOS;
//    k7b — PIC, IDT, ассемблерные входы (int 0x80, IRQ, ловушки);
//    k8  — планировщик задач (вытесняющий, циклический);
//    k11 — системные вызовы (единственная дверь из ring 3);
//    k9  — заставка; k10 — точка входа и загрузка оболочки с диска.
//
//  Вся графическая оболочка (рабочий стол, окна, консоль, приложения,
//  игра, редактор, мастер установки) — ОТДЕЛЬНАЯ программа shell.bin,
//  она собирается отдельно (см. shell/shell.cpp) и грузится с MyFS.
// ===========================================================================

#include "../common/myos.h"

// Порядок важен: каждый следующий модуль опирается на предыдущие.
#include "k0_io.cpp"        // порты, лог, печать, mem/str, user_ok
#include "k1_fb.cpp"        // видеопамять, примитивы, шрифт (+ ../font_data.inc)
#include "k2_ata.cpp"       // ATA PIO + disk_buf
#include "k3_myfs.cpp"      // MyFS: суперблок/таблица/поиск раздела
#include "k3b_fsops.cpp"    // MyFS: format/ls/write/delete/rename/cat
#include "k3c_fsread.cpp"   // MyFS: file_list, fs_load_table, чтение файлов
#include "k4_parts.cpp"     // разделы: cmd_part / cmd_mkpart / cmd_wtest
#include "k4b_install.cpp"  // установка: install_check/plan/do
#include "k4c_boot.cpp"     // «записка» VBR + поиск раздела с файлом системы
#include "k6_input.cpp"     // PS/2: очереди клавиатуры/мыши, IRQ1/IRQ12
#include "k7_cpu.cpp"       // page_dir, mem_init, gdt_init, idt_set
#include "k7_time.cpp"      // PIT 100 Гц, millis, часы CMOS
#include "k8_task.cpp"      // задачи: task_create/switch/exit (+ task_switch asm)
#include "k7b_irq.cpp"      // PIC, IDT, стабы int 0x80/IRQ/ловушек
#include "k11_sys.cpp"      // диспетчер системных вызовов
#include "k9_splash.cpp"    // заставка: большой шрифт + полоса загрузки
#include "k10_kmain.cpp"    // kernel_main: загрузка оболочки и запуск задачи

// ELF-тулчейн (i686-elf-gcc, Linux/macOS) не добавляет '_' к именам C-функций,
// а ассемблерные стабы в k7b/k8 написаны в расчёте на MinGW/COFF. Даём
// недостающие алиасы, чтобы ядро собиралось обоими тулчейнами.
#if defined(__ELF__)
__asm__(
    ".globl isr_syscall\n.set isr_syscall, _isr_syscall\n"
    ".globl isr_irq0\n.set isr_irq0, _isr_irq0\n"
    ".globl isr_irq1\n.set isr_irq1, _isr_irq1\n"
    ".globl isr_irq12\n.set isr_irq12, _isr_irq12\n"
    ".globl irq_exit\n.set irq_exit, _irq_exit\n"
    ".globl isr_df\n.set isr_df, _isr_df\n"
    ".globl isr_gp\n.set isr_gp, _isr_gp\n"
    ".globl isr_pf\n.set isr_pf, _isr_pf\n"
    ".globl task_switch\n.set task_switch, _task_switch\n"
    ".globl jump_to_task\n.set jump_to_task, _jump_to_task\n"
    ".globl _syscall_dispatch\n.set _syscall_dispatch, syscall_dispatch\n"
    ".globl _irq_dispatch\n.set _irq_dispatch, irq_dispatch\n"
    ".globl _cpu_fault\n.set _cpu_fault, cpu_fault\n"
);
#endif
