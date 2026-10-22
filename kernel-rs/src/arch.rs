// ===========================================================================
//  arch — архитектура x86: порты, COM1, GDT/TSS, IDT/PIC/PIT, страницы,
//         ассемблерные входы в ядро (int 0x80, IRQ, ловушки)
// ===========================================================================
//  Всё «голое железо» собрано здесь. Остальные модули зовут безопасные
//  обёртки и не знают, как именно устроен процессор.
// ===========================================================================

use core::arch::{asm, global_asm};

// --- Порты ввода-вывода ----------------------------------------------------
#[inline(always)]
pub unsafe fn outb(port: u16, value: u8) {
    asm!("out dx, al", in("dx") port, in("al") value,
         options(nomem, nostack, preserves_flags));
}

#[inline(always)]
pub unsafe fn inb(port: u16) -> u8 {
    let v: u8;
    asm!("in al, dx", in("dx") port, out("al") v,
         options(nomem, nostack, preserves_flags));
    v
}

#[inline(always)]
pub unsafe fn outw(port: u16, value: u16) {
    asm!("out dx, ax", in("dx") port, in("ax") value,
         options(nomem, nostack, preserves_flags));
}

#[inline(always)]
pub unsafe fn inw(port: u16) -> u16 {
    let v: u16;
    asm!("in ax, dx", in("dx") port, out("ax") v,
         options(nomem, nostack, preserves_flags));
    v
}

#[inline(always)]
pub unsafe fn hlt() {
    asm!("hlt", options(nomem, nostack, preserves_flags));
}

#[inline(always)]
pub unsafe fn pause() {
    asm!("pause", options(nomem, nostack, preserves_flags));
}

#[inline(always)]
pub unsafe fn cli() {
    asm!("cli", options(nomem, nostack));
}

#[inline(always)]
pub unsafe fn sti() {
    asm!("sti", options(nomem, nostack));
}

// --- COM1: отладочный порт (serial) ----------------------------------------
const COM1: u16 = 0x3F8;

pub fn serial_init() {
    unsafe {
        outb(COM1 + 1, 0x00);
        outb(COM1 + 3, 0x80);
        outb(COM1 + 0, 0x03);
        outb(COM1 + 1, 0x00);
        outb(COM1 + 3, 0x03);
        outb(COM1 + 2, 0xC7);
        outb(COM1 + 4, 0x0B);
    }
}

pub fn serial_putc(c: u8) {
    unsafe {
        for _ in 0..10000 {
            if inb(COM1 + 5) & 0x20 != 0 {
                break;
            }
        }
        outb(COM1, c);
    }
}

pub fn serial_write(s: &[u8]) {
    for &b in s {
        serial_putc(b);
    }
}

// --- GDT + TSS --------------------------------------------------------------
#[repr(C, packed)]
struct DescPtr {
    limit: u16,
    base: u32,
}

// TSS — «шпаргалка» процессора: откуда брать стек ядра (ss0/esp0), когда
// программа ring 3 входит в ring 0. Ровно 104 байта, поля строго по порядку.
#[repr(C, packed)]
pub struct Tss {
    prev: u32,
    esp0: u32,
    ss0: u32,
    esp1: u32,
    ss1: u32,
    esp2: u32,
    ss2: u32,
    cr3: u32,
    eip: u32,
    eflags: u32,
    eax: u32,
    ecx: u32,
    edx: u32,
    ebx: u32,
    esp: u32,
    ebp: u32,
    esi: u32,
    edi: u32,
    es: u32,
    cs: u32,
    ss: u32,
    ds: u32,
    fs: u32,
    gs: u32,
    ldt: u32,
    trap: u16,
    iomap: u16,
}

const TSS_SIZE: usize = 104;

#[repr(align(16))]
struct GdtTab([u8; 48]); // 6 сегментов

#[repr(align(16))]
struct IdtTab([u8; 2048]); // 256 шлюзов

static mut GDT: GdtTab = GdtTab([0; 48]);
static mut IDT: IdtTab = IdtTab([0; 2048]);
static mut TSS: Tss = Tss {
    prev: 0, esp0: 0, ss0: 0, esp1: 0, ss1: 0, esp2: 0, ss2: 0,
    cr3: 0, eip: 0, eflags: 0, eax: 0, ecx: 0, edx: 0, ebx: 0,
    esp: 0, ebp: 0, esi: 0, edi: 0, es: 0, cs: 0, ss: 0, ds: 0,
    fs: 0, gs: 0, ldt: 0, trap: 0, iomap: 0,
};

// Стек ядра для первых входов (дальше у каждой задачи свой).
static mut SYS_STACK: [u32; 512] = [0; 512];

unsafe fn gdt_ptr() -> *mut u8 {
    core::ptr::addr_of_mut!(GDT.0) as *mut u8
}

fn gdt_set(i: usize, base: u32, limit: u32, access: u8, flags: u8) {
    unsafe {
        let d = gdt_ptr().add(i * 8);
        d.write((limit & 0xFF) as u8);
        d.add(1).write(((limit >> 8) & 0xFF) as u8);
        d.add(2).write((base & 0xFF) as u8);
        d.add(3).write(((base >> 8) & 0xFF) as u8);
        d.add(4).write(((base >> 16) & 0xFF) as u8);
        d.add(5).write(access);
        d.add(6).write((((limit >> 16) & 0x0F) as u8) | flags);
        d.add(7).write(((base >> 24) & 0xFF) as u8);
    }
}

pub fn gdt_init() {
    unsafe {
        core::ptr::write_bytes(gdt_ptr(), 0, 48);
        gdt_set(1, 0, 0xFFFFF, 0x9A, 0xCF); // 0x08 — код ядра
        gdt_set(2, 0, 0xFFFFF, 0x92, 0xCF); // 0x10 — данные ядра
        gdt_set(3, 0, 0xFFFFF, 0xFA, 0xCF); // 0x1B — код программы (ring 3)
        gdt_set(4, 0, 0xFFFFF, 0xF2, 0xCF); // 0x23 — данные программы

        // TSS: стек ядра — сегмент данных ядра, I/O-карты нет.
        let mut t: Tss = core::mem::zeroed();
        t.ss0 = 0x10;
        t.esp0 = core::ptr::addr_of!(SYS_STACK) as u32 + 512 * 4;
        t.iomap = TSS_SIZE as u16;
        TSS = t;
        gdt_set(5, core::ptr::addr_of!(TSS) as u32, (TSS_SIZE - 1) as u32, 0x89, 0);

        let gdtr = DescPtr {
            limit: 47,
            base: gdt_ptr() as u32,
        };
        asm!("lgdt [{}]", in(reg) &gdtr, options(readonly, nostack, preserves_flags));

        // Перезагрузить сегменты данных (CS уже 0x08 — дескриптор тот же).
        asm!(
            "mov ds, {0:x}", "mov es, {0:x}", "mov fs, {0:x}",
            "mov gs, {0:x}", "mov ss, {0:x}",
            in(reg) 0x10u16,
            options(nomem, nostack, preserves_flags)
        );
        asm!("ltr {0:x}", in(reg) 0x28u16, options(nostack, preserves_flags));
    }
}

pub unsafe fn tss_set_esp0(top: u32) {
    let p = core::ptr::addr_of_mut!(TSS) as *mut u8;
    (p.add(4) as *mut u32).write_volatile(top);
}

// --- IDT + PIC + PIT --------------------------------------------------------
unsafe fn idt_ptr() -> *mut u8 {
    core::ptr::addr_of_mut!(IDT.0) as *mut u8
}

fn idt_set(n: usize, handler: u32, dpl: u8) {
    unsafe {
        let d = idt_ptr().add(n * 8);
        core::ptr::write_bytes(d, 0, 8);
        if handler == 0 {
            return;
        }
        d.write((handler & 0xFF) as u8);
        d.add(1).write(((handler >> 8) & 0xFF) as u8);
        d.add(2).write(0x08);
        d.add(5).write(0x8E | (dpl << 5));
        d.add(6).write(((handler >> 16) & 0xFF) as u8);
        d.add(7).write(((handler >> 24) & 0xFF) as u8);
    }
}

extern "C" {
    fn isr_syscall();
    fn isr_irq0();
    fn isr_irq1();
    fn isr_irq12();
    fn isr_df();
    fn isr_gp();
    fn isr_pf();
}

pub fn idt_init() {
    unsafe {
        idt_set(0x80, isr_syscall as usize as u32, 3);
        idt_set(8, isr_df as usize as u32, 0);
        idt_set(13, isr_gp as usize as u32, 0);
        idt_set(14, isr_pf as usize as u32, 0);
        idt_set(32, isr_irq0 as usize as u32, 0);
        idt_set(33, isr_irq1 as usize as u32, 0);
        idt_set(44, isr_irq12 as usize as u32, 0);
        let idtr = DescPtr {
            limit: 2047,
            base: idt_ptr() as u32,
        };
        asm!("lidt [{}]", in(reg) &idtr, options(readonly, nostack, preserves_flags));
    }
}

pub fn pic_init() {
    unsafe {
        outb(0x20, 0x11); outb(0x80, 0);
        outb(0xA0, 0x11); outb(0x80, 0);
        outb(0x21, 0x20); outb(0x80, 0);
        outb(0xA1, 0x28); outb(0x80, 0);
        outb(0x21, 0x04); outb(0x80, 0);
        outb(0xA1, 0x02); outb(0x80, 0);
        outb(0x21, 0x01); outb(0x80, 0);
        outb(0xA1, 0x01); outb(0x80, 0);
        outb(0x21, 0xF8); // IRQ0/1/2
        outb(0xA1, 0xEF); // только IRQ12
    }
}

static mut TIMER_TICKS: u32 = 0;

pub fn pit_init() {
    unsafe {
        let div: u32 = 1193182 / 100;
        outb(0x43, 0x36);
        outb(0x40, (div & 0xFF) as u8);
        outb(0x40, ((div >> 8) & 0xFF) as u8);
        TIMER_TICKS = 0;
    }
}

pub fn millis() -> u32 {
    unsafe { core::ptr::read_volatile(core::ptr::addr_of!(TIMER_TICKS)) * 10 }
}

fn timer_tick() {
    unsafe {
        let p = core::ptr::addr_of_mut!(TIMER_TICKS);
        p.write_volatile(p.read_volatile().wrapping_add(1));
    }
    crate::task::timer_tick();
}

// --- CMOS: секунды с начала суток ------------------------------------------
fn cmos_read(reg: u8) -> u8 {
    unsafe {
        outb(0x70, reg);
        inb(0x71)
    }
}

fn bcd2bin(b: u8) -> u32 {
    (b & 0x0F) as u32 + ((b >> 4) as u32) * 10
}

pub fn day_secs() -> u32 {
    bcd2bin(cmos_read(4)) * 3600 + bcd2bin(cmos_read(2)) * 60 + bcd2bin(cmos_read(0))
}

// --- Страницы: PSE (страницы по 4 МБ) --------------------------------------
#[repr(align(4096))]
struct PageDir([u32; 1024]);

static mut PAGE_DIR: PageDir = PageDir([0; 1024]);

const PTE_P: u32 = 0x1;
const PTE_RW: u32 = 0x2;
const PTE_USER: u32 = 0x4;
const PTE_PS: u32 = 0x80;

/// Разрешить страницы и отдать программам 4..8 МБ, а также видеопамять.
pub fn paging_init(fb_addr: u32) {
    unsafe {
        let pd = core::ptr::addr_of_mut!(PAGE_DIR.0) as *mut u32;
        for i in 0..1024 {
            pd.add(i).write_volatile(0);
        }
        pd.write_volatile(0 | PTE_P | PTE_RW | PTE_PS); // 0..4 МБ — ядро
        pd.add(1).write_volatile(0x0040_0000 | PTE_P | PTE_RW | PTE_PS | PTE_USER);

        let base = fb_addr & 0xFFC0_0000;
        for i in 0..3usize {
            let idx = ((base >> 22) as usize) + i;
            pd.add(idx)
                .write_volatile((base + (i as u32) * 0x10_0000) | PTE_P | PTE_RW | PTE_PS | PTE_USER);
        }

        // FPU/SSE уже включены ассемблерным входом _kernel_main; повторяем
        // здесь, чтобы функция была самодостаточной. Порядок инструкций
        // безопасен: OSFXSR выставлен ещё до первого Rust-кода.
        let mut cr0: u32;
        asm!("mov {}, cr0", out(reg) cr0, options(nomem, nostack, preserves_flags));
        cr0 &= !0x0C;   // EM = 0, TS = 0
        cr0 |= 0x22;    // MP + NE
        asm!("mov cr0, {}", in(reg) cr0, options(nomem, nostack));

        let mut cr4: u32;
        asm!("mov {}, cr4", out(reg) cr4, options(nomem, nostack, preserves_flags));
        cr4 |= 0x610;   // PSE | OSFXSR | OSXMMEXCPT
        asm!("mov cr4, {}", in(reg) cr4, options(nomem, nostack));

        asm!("mov cr3, {}", in(reg) pd as u32, options(nomem, nostack));

        let mut cr0b: u32;
        asm!("mov {}, cr0", out(reg) cr0b, options(nomem, nostack, preserves_flags));
        cr0b |= 0x8000_0000; // PG
        asm!("mov cr0, {}", in(reg) cr0b, options(nomem, nostack));
    }
}

// ===========================================================================
//  Ассемблерные входы в ядро
// ===========================================================================
//  Кадр (как в C++ v2.x): CPU кладёт EIP,CS,EFLAGS(,UESP,SS), стаб — err,vec,
//  pushad, ds/es/fs/gs. В памяти от младшего адреса:
//    GS,FS,ES,DS,EDI,ESI,EBP,ESP,EBX,EDX,ECX,EAX,VEC,ERR,EIP,CS,EFLAGS,(UESP,SS)
// ===========================================================================
global_asm!(
    ".text",
    // --- int 0x80: системный вызов из ring 3 ---
    ".global _isr_syscall",
    "_isr_syscall:",
    "    push 0",
    "    push 0x80",
    "    pushad",
    // Сегменты кладём вручную по 4 байта: gas для `push ds` выдаёт
    // 16-битный pushw (66 1e) и сдвигает весь кадр. EAX уже сохранён pushad.
    "    xor eax, eax",
    "    mov ax, ds",
    "    push eax",
    "    xor eax, eax",
    "    mov ax, es",
    "    push eax",
    "    xor eax, eax",
    "    mov ax, fs",
    "    push eax",
    "    xor eax, eax",
    "    mov ax, gs",
    "    push eax",
    "    mov ax, 0x10",
    "    mov ds, ax",
    "    mov es, ax",
    "    mov fs, ax",
    "    mov gs, ax",
    "    push esp",
    "    call _syscall_dispatch",
    "    add esp, 4",
    "    jmp _irq_exit",

    // --- IRQ0 / IRQ1 / IRQ12 ---
    ".global _isr_irq0",
    "_isr_irq0:",
    "    push 0",
    "    push 32",
    "    jmp irq_common",
    ".global _isr_irq1",
    "_isr_irq1:",
    "    push 0",
    "    push 33",
    "    jmp irq_common",
    ".global _isr_irq12",
    "_isr_irq12:",
    "    push 0",
    "    push 44",
    "irq_common:",
    "    pushad",
    "    xor eax, eax",
    "    mov ax, ds",
    "    push eax",
    "    xor eax, eax",
    "    mov ax, es",
    "    push eax",
    "    xor eax, eax",
    "    mov ax, fs",
    "    push eax",
    "    xor eax, eax",
    "    mov ax, gs",
    "    push eax",
    "    mov ax, 0x10",
    "    mov ds, ax",
    "    mov es, ax",
    "    mov fs, ax",
    "    mov gs, ax",
    "    push esp",
    "    call _irq_dispatch",
    "    add esp, 4",

    // --- общий хвост: возврат в прерванный код (и старт новых задач) ---
    ".global _irq_exit",
    "_irq_exit:",
    "    pop eax",
    "    mov gs, ax",
    "    pop eax",
    "    mov fs, ax",
    "    pop eax",
    "    mov es, ax",
    "    pop eax",
    "    mov ds, ax",
    "    popad",
    "    add esp, 8",
    // 32-битный iret: gas для `iret` выдаёт iretw (66 cf). 0xcf — iretd.
    "    .byte 0xcf",

    // --- ловушки CPU: #DF/#GP/#PF (ошибка уже на стеке) ---
    ".global _isr_df",
    "_isr_df:",
    "    push 8",
    "    jmp fault_common",
    ".global _isr_gp",
    "_isr_gp:",
    "    push 13",
    "    jmp fault_common",
    ".global _isr_pf",
    "_isr_pf:",
    "    push 14",
    "fault_common:",
    "    pushad",
    "    xor eax, eax",
    "    mov ax, ds",
    "    push eax",
    "    xor eax, eax",
    "    mov ax, es",
    "    push eax",
    "    xor eax, eax",
    "    mov ax, fs",
    "    push eax",
    "    xor eax, eax",
    "    mov ax, gs",
    "    push eax",
    "    mov ax, 0x10",
    "    mov ds, ax",
    "    mov es, ax",
    "    mov fs, ax",
    "    mov gs, ax",
    "    push esp",
    "    call _cpu_fault",
    "    add esp, 4",
    "1:  hlt",
    "    jmp 1b",
);

// --- Обработчик прерываний: EOI и диспетчеризация -------------------------
#[no_mangle]
pub unsafe extern "C" fn irq_dispatch(regs: *mut u32) {
    let vec = regs.add(12).read();
    if vec == 32 {
        outb(0x20, 0x20);
        timer_tick();
        // Вытесняем только ring 3 (CS & 3 == 3), в ядре прерывания запрещены.
        if (regs.add(15).read() & 3) == 3 {
            crate::task::schedule_from_irq();
        }
        return;
    }
    if vec == 33 {
        outb(0x20, 0x20);
        crate::input::kbd_irq();
        return;
    }
    if vec == 44 {
        outb(0xA0, 0x20);
        outb(0x20, 0x20);
        crate::input::mouse_irq();
        return;
    }
    outb(0x20, 0x20);
}

// --- Ловушка CPU: диагноз в dmesg и остановка ------------------------------
#[no_mangle]
pub unsafe extern "C" fn cpu_fault(regs: *mut u32) {
    let vec = regs.add(12).read();
    let err = regs.add(13).read();
    let eip = regs.add(14).read();
    let mut cr2: u32;
    asm!("mov {}, cr2", out(reg) cr2, options(nomem, nostack, preserves_flags));
    crate::log::puts(b"CPU fault: vector ");
    crate::log::print_dec(vec);
    crate::log::puts(b", error ");
    crate::log::print_dec(err);
    crate::log::puts(b" at eip=");
    crate::log::print_dec(eip);
    crate::log::puts(b" addr=");
    crate::log::print_dec(cr2);
    crate::log::puts(b"\nSystem halted.\n");
    loop {
        hlt();
    }
}
