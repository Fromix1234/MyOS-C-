// ===========================================================================
//  MyOS v3 — ЯДРО НА RUST (ring 0), новая архитектура с нуля
// ===========================================================================
//  Принципы:
//    * монолитное ядро, но с ЖЁСТКИМИ границами модулей (arch / log / video /
//      input / disk / task / syscall) — каждый модуль прячет своё состояние;
//    * никаких общих переменных с оболочкой: только ABI системных вызовов
//      (common/myos.h) — оболочка осталась на C++ в ring 3;
//    * загрузчик (boot/vbr/stage2, NASM) не меняется: он грузит kernel.bin
//      по адресу 0x10000 и прыгает на kernel_main;
//    * unsafe — только в низкоуровневых модулях (порты, таблицы, MMIO).
//
//  Этот файл — точка входа и корень дерева модулей.
// ===========================================================================

#![no_std]
#![allow(static_mut_refs)]

use core::panic::PanicInfo;

mod abi;
mod arch;
mod disk;
mod font;
mod input;
mod log;
mod splash;
mod syscall;
mod task;
mod video;

// ===========================================================================
//  Точка входа (ENTRY _kernel_main): включаем FPU/SSE ДО первого Rust-кода.
// ===========================================================================
//  Компилятор вправе вставить SSE-инструкции (xorps/movaps) в пролог любой
//  функции — даже в ту, которая сама эти биты и выставляет. Без CR4.OSFXSR
//  первая же такая инструкция даёт #UD, а IDT ещё не загружена → тройной
//  сбой. Поэтому CR0/CR4 готовим на ассемблере и только потом зовём Rust.
core::arch::global_asm!(
    ".text",
    ".global _kernel_main",
    "_kernel_main:",
    "    mov eax, cr0",
    "    and eax, 0xFFFFFFF3",   // EM=0, TS=0
    "    or  eax, 0x00000022",   // MP | NE
    "    mov cr0, eax",
    "    mov eax, cr4",
    "    or  eax, 0x00000610",   // PSE | OSFXSR | OSXMMEXCPT
    "    mov cr4, eax",
    "    call _kernel_main_rust",
    "1:  hlt",
    "    jmp 1b",
);

// Символы линкера: границы .bss (см. kernel-rs/kernel.ld).
extern "C" {
    #[link_name = "_bss_start"]
    static BSS_START: u8;
    #[link_name = "_bss_end"]
    static BSS_END: u8;
}

/// Обнулить .bss: в плоский образ она не входит (stage2 грузит только .text),
/// поэтому ядро само приводит свои статические данные в исходное состояние.
fn zero_bss() {
    unsafe {
        let mut p = core::ptr::addr_of!(BSS_START) as usize as *mut u8;
        let end = core::ptr::addr_of!(BSS_END) as usize;
        while (p as usize) < end {
            p.write_volatile(0);
            p = p.add(1);
        }
    }
}

/// Паника ядра: печатаем в COM1 и навсегда останавливаемся.
#[panic_handler]
fn panic(_info: &PanicInfo) -> ! {
    arch::serial_write(b"kernel panic: stopped\n");
    log::puts(b"kernel panic: stopped\n");
    loop {
        unsafe { arch::hlt() }
    }
}

/// Точка входа Rust-части: вызывается из ассемблерного стаба _kernel_main
/// (он уже включил FPU/SSE).
#[no_mangle]
pub extern "C" fn kernel_main_rust() -> ! {
    unsafe {
        arch::serial_init();
        zero_bss();
        log::reset();
        log::puts(b"boot: kernel start (MyOS v3.0, Rust)\n");

        // Видео: режим уже поставил stage2, ядро лишь читает «записку».
        video::init_video();

        // Память, сегменты, прерывания, таймер, ввод.
        arch::paging_init(video::fb_addr());
        arch::gdt_init();
        arch::idt_init();
        arch::pic_init();
        arch::pit_init();
        input::ps2_init();
        arch::sti();

        // После sti добиваем «залётные» байты PS/2 (ACK/самотест), чтобы они
        // не попали в очередь клавиш и не пропустили заставку раньше времени.
        let t0 = arch::millis();
        while arch::millis().wrapping_sub(t0) < 30 {
            while arch::inb(0x64) & 1 != 0 {
                let _ = arch::inb(0x60);
            }
            arch::pause();
        }
        input::kbd_reset();
        input::mouse_reset();

        splash::draw_splash();

        // Откуда загрузились — из «записки» VBR (0x0500/0x0502).
        let (bp, _lba) = disk::boot_partition();
        let mut sys_part = if bp >= 1 { bp } else { 0 };

        // Снимки файлов системы: 1 МБ shell.bin (0x100000) + 1 МБ demo.bin.
        let mut shell_len: i32 = -1;
        if sys_part > 0 {
            shell_len = disk::fs_read_by_name(
                sys_part,
                b"shell.bin",
                disk::SHELL_SNAP as *mut u8,
                0x10_0000,
            );
        }
        if shell_len <= 0 {
            // Загрузочный раздел может быть MyFS без файлов (после install):
            // честно ищем оболочку на любом разделе.
            let n2 = disk::find_part_with_file(b"shell.bin");
            if n2 > 0 {
                sys_part = n2;
                shell_len = disk::fs_read_by_name(
                    sys_part,
                    b"shell.bin",
                    disk::SHELL_SNAP as *mut u8,
                    0x10_0000,
                );
            }
        }
        core::ptr::write_volatile(core::ptr::addr_of_mut!(disk::SYS_PART), sys_part);
        core::ptr::write_volatile(
            core::ptr::addr_of_mut!(disk::SHELL_SNAP_LEN),
            if shell_len > 0 { shell_len as u32 } else { 0 },
        );

        let demo_len = if sys_part > 0 {
            disk::fs_read_by_name(
                sys_part,
                b"demo.bin",
                disk::DEMO_SNAP as *mut u8,
                0x10_0000,
            )
        } else {
            -1
        };
        core::ptr::write_volatile(
            core::ptr::addr_of_mut!(disk::DEMO_SNAP_LEN),
            if demo_len > 0 { demo_len as u32 } else { 0 },
        );

        log::puts(b"boot: P");
        log::print_dec(if sys_part > 0 { sys_part as u32 } else { 0 });
        log::puts(b", shell.bin = ");
        log::print_dec(if shell_len > 0 { shell_len as u32 } else { 0 });
        log::puts(b" bytes, demo.bin = ");
        log::print_dec(if demo_len > 0 { demo_len as u32 } else { 0 });
        log::puts(b" bytes\n");

        if shell_len <= 0 {
            splash::boot_error_screen(b"shell.bin not found on any MyFS partition");
        }

        // Зона оболочки 4..5 МБ — чистая (как mmap со свежими страницами),
        // затем копируем туда снимок shell.bin.
        let mut i = 0u32;
        while i < 0x10_0000 {
            core::ptr::write_volatile((0x0040_0000 + i) as *mut u32, 0);
            i += 4;
        }
        core::ptr::copy_nonoverlapping(
            disk::SHELL_SNAP as *const u8,
            0x0040_0000 as *mut u8,
            shell_len as usize,
        );

        let pid = task::create(b"shell", 0x0040_0000, 0x004F_FFF0, 0);
        if pid < 1 {
            splash::boot_error_screen(b"cannot create shell task");
        }
        task::set_input_owner(pid);
        task::set_current(pid);
        arch::tss_set_esp0(task::kstack_top(pid));
        log::puts(b"boot: entering shell (ring 3)\n");
        task::jump_to(task::esp_of(pid));
    }
}

// ===========================================================================
//  Свои mem/str-функции: ядро линкуется без CRT, а кодогенератор Rust вправе
//  звать memcpy/memset. Пишем байтовыми volatile-циклами, чтобы компилятор
//  не «оптимизировал» их обратно в вызов самих себя.
// ===========================================================================
#[no_mangle]
pub unsafe extern "C" fn memcpy(dst: *mut u8, src: *const u8, n: usize) -> *mut u8 {
    let mut i = 0usize;
    while i < n {
        dst.add(i).write_volatile(src.add(i).read_volatile());
        i += 1;
    }
    dst
}

#[no_mangle]
pub unsafe extern "C" fn memmove(dst: *mut u8, src: *const u8, n: usize) -> *mut u8 {
    if (dst as usize) < (src as usize) {
        let mut i = 0usize;
        while i < n {
            dst.add(i).write_volatile(src.add(i).read_volatile());
            i += 1;
        }
    } else {
        let mut i = n;
        while i > 0 {
            i -= 1;
            dst.add(i).write_volatile(src.add(i).read_volatile());
        }
    }
    dst
}

#[no_mangle]
pub unsafe extern "C" fn memset(dst: *mut u8, c: i32, n: usize) -> *mut u8 {
    let b = c as u8;
    let mut i = 0usize;
    while i < n {
        dst.add(i).write_volatile(b);
        i += 1;
    }
    dst
}

#[no_mangle]
pub unsafe extern "C" fn memcmp(a: *const u8, b: *const u8, n: usize) -> i32 {
    let mut i = 0usize;
    while i < n {
        let x = a.add(i).read_volatile();
        let y = b.add(i).read_volatile();
        if x != y {
            return x as i32 - y as i32;
        }
        i += 1;
    }
    0
}

#[no_mangle]
pub unsafe extern "C" fn bcmp(a: *const u8, b: *const u8, n: usize) -> i32 {
    memcmp(a, b, n)
}
