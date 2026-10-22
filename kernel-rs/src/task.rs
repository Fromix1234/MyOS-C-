// ===========================================================================
//  task — планировщик: вытесняющая многозадачность (циклическая)
// ===========================================================================
//  У каждой задачи СВОЙ стек ядра; процессор переключается на него через
//  TSS.esp0 при входе в ring 0. Таймер (IRQ0, каждый 2-й тик ≈ 20 мс)
//  сохраняет контекст текущей задачи и прыгает в следующую.
//
//  Новая задача стартует «из прерывания»: на её стеке собран кадр, который
//  выглядит как результат работы стаба (arch::isr_*), и адрес _irq_exit —
//  первый же iret отправляет её в ring 3 на заданный вход.
// ===========================================================================

use core::arch::global_asm;

use crate::arch;

pub const TASK_MAX: usize = 4;

#[derive(Clone, Copy)]
struct Task {
    esp: u32,        // сохранённый ESP ядра (0 = нет контекста)
    kstack_top: u32, // вершина стека ядра (кладём в TSS.esp0)
    state: i32,      // 0 — свободна, 1 — жива, 2 — завершилась
    parent: i32,     // pid родителя (1-based), 0 — нет
    name: [u8; 16],
}

static mut TASKS: [Task; TASK_MAX] = [Task {
    esp: 0,
    kstack_top: 0,
    state: 0,
    parent: 0,
    name: [0; 16],
}; TASK_MAX];

static mut TASK_COUNT: i32 = 0; // сколько слотов когда-либо занято (для pid)
static mut CUR_TASK: i32 = 0; // индекс текущей задачи (pid = индекс + 1)
static mut INPUT_OWNER: i32 = 1; // чей pid получает КЛАВИШИ

pub static mut TIMER_SLICE: u32 = 0;

// Стек ядра для задачи (4 КБ — хватает: ядро короткое, окна рисует оболочка).
static mut KSTACKS: [[u32; 1024]; TASK_MAX] = [[0; 1024]; TASK_MAX];

extern "C" {
    fn task_switch(save: *mut u32, new_esp: u32);
    fn jump_to_task(new_esp: u32) -> !;
    fn irq_exit();
}

global_asm!(
    ".text",
    // --- переключение контекста (callee-saved + подмена стека) ---
    ".global _task_switch",
    "_task_switch:",
    "    push ebp",
    "    push ebx",
    "    push edi",
    "    push esi",
    "    mov eax, [esp+20]", // save_esp (16 байт наших push + адрес возврата)
    "    mov edx, [esp+24]", // new_esp
    "    mov [eax], esp", // сохранили свой контекст
    "    mov esp, edx", // перешли на чужой
    "    pop esi",
    "    pop edi",
    "    pop ebx",
    "    pop ebp",
    "    ret",
    // --- первый запуск задачи из ядра (kernel_main -> ring 3) ---
    ".global _jump_to_task",
    "_jump_to_task:",
    "    mov esp, [esp+4]",
    "    pop esi",
    "    pop edi",
    "    pop ebx",
    "    pop ebp",
    "    ret",
);

unsafe fn task_at(i: usize) -> *mut Task {
    (core::ptr::addr_of_mut!(TASKS) as *mut Task).add(i)
}

fn task_alloc() -> i32 {
    for i in 0..TASK_MAX {
        if unsafe { (*task_at(i)).state } != 1 {
            return i as i32;
        }
    }
    -1
}

/// Создать задачу: entry — адрес кода (ring 3), ustack — вершина стека
/// программы. Возврат: pid (1..4) или -1.
pub fn create(name: &[u8], entry: u32, ustack: u32, parent: i32) -> i32 {
    let i = task_alloc();
    if i < 0 {
        return -1;
    }
    let i = i as usize;
    unsafe {
        let t = (core::ptr::addr_of!(KSTACKS) as *const u32).add(i * 1024 + 1024);
        let mut sp = t as *mut u32;

        // Собираем кадр СВЕРХУ ВНИЗ (адреса убывают). Порядок обязан совпадать
        // с arch::isr_*: [SS][UESP][EFLAGS][CS][EIP][ERR][VEC][DS][ES][FS][GS]
        // [pushal], затем адрес возврата _irq_exit и «сохранённые» ESI/EDI/EBX/EBP.
        macro_rules! push {
            ($v:expr) => {{
                sp = sp.sub(1);
                sp.write($v);
            }};
        }
        push!(0x23u32); // SS — сегмент данных программы
        push!(ustack); // UESP программы
        push!(0x202u32); // EFLAGS (IF=1 — прерывания разрешены)
        push!(0x1Bu32); // CS — сегмент кода программы (ring 3)
        push!(entry); // EIP — вход программы
        push!(0u32); // ERR
        push!(32u32); // VEC (как будто тик таймера)
        push!(0x23u32); // DS
        push!(0x23u32); // ES
        push!(0x23u32); // FS
        push!(0x23u32); // GS
        for _ in 0..8 {
            push!(0u32); // pushal (EAX..EDI — мы вышли из iret)
        }
        push!(irq_exit as usize as u32); // «возврат» стаба
        push!(0u32); // EBP (task_switch pop-ит их в обратном порядке)
        push!(0u32); // EBX
        push!(0u32); // EDI
        push!(0u32); // ESI

        let task = task_at(i);
        (*task).esp = sp as u32;
        (*task).kstack_top = t as u32;
        (*task).state = 1;
        (*task).parent = parent;
        let mut n = 0;
        while n < 15 && n < name.len() && name[n] != 0 {
            (*task).name[n] = name[n];
            n += 1;
        }
        (*task).name[n] = 0;

        let count = core::ptr::read_volatile(core::ptr::addr_of!(TASK_COUNT));
        if i as i32 + 1 > count {
            core::ptr::write_volatile(core::ptr::addr_of_mut!(TASK_COUNT), i as i32 + 1);
        }
    }
    i as i32 + 1
}

unsafe fn switch_to(next: i32) {
    let old = core::ptr::read_volatile(core::ptr::addr_of!(CUR_TASK));
    if next == old || next < 0 || next >= TASK_MAX as i32 {
        return;
    }
    let next_t = task_at(next as usize);
    if (*next_t).state != 1 {
        return;
    }
    core::ptr::write_volatile(core::ptr::addr_of_mut!(CUR_TASK), next);
    arch::tss_set_esp0((*next_t).kstack_top);
    let old_t = task_at(old as usize);
    task_switch(core::ptr::addr_of_mut!((*old_t).esp), (*next_t).esp);
}

/// Каждые 2 тика (≈20 мс) — следующей живой задаче.
pub fn schedule_from_irq() {
    unsafe {
        let count = core::ptr::read_volatile(core::ptr::addr_of!(TASK_COUNT));
        if count < 2 {
            return;
        }
        if (TIMER_SLICE & 1) == 0 {
            return;
        }
        let cur = core::ptr::read_volatile(core::ptr::addr_of!(CUR_TASK));
        for k in 1..=count {
            let j = (cur + k) % count;
            if (*task_at(j as usize)).state == 1 {
                switch_to(j);
                return;
            }
        }
    }
}

/// Уступить процессор немедленно (SYS_YIELD, SYS_EXIT).
pub fn schedule_now() {
    unsafe {
        let count = core::ptr::read_volatile(core::ptr::addr_of!(TASK_COUNT));
        if count < 2 {
            return;
        }
        let cur = core::ptr::read_volatile(core::ptr::addr_of!(CUR_TASK));
        for k in 1..=count {
            let j = (cur + k) % count;
            if (*task_at(j as usize)).state == 1 {
                switch_to(j);
                return;
            }
        }
    }
}

/// Завершить задачу pid: ввод возвращается родителю, задача «убита».
pub fn exit(pid: i32) {
    if pid < 1 || pid > TASK_MAX as i32 {
        return;
    }
    unsafe {
        let t = task_at((pid - 1) as usize);
        (*t).state = 2;
        let owner = core::ptr::read_volatile(core::ptr::addr_of!(INPUT_OWNER));
        if owner == pid {
            let back = if (*t).parent > 0 { (*t).parent } else { 1 };
            core::ptr::write_volatile(core::ptr::addr_of_mut!(INPUT_OWNER), back);
        }
    }
    schedule_now();
    // Если живых больше нет — вечно спим (эту задачу уже не разбудят).
    loop {
        unsafe { arch::hlt() };
    }
}

pub fn timer_tick() {
    unsafe {
        let p = core::ptr::addr_of_mut!(TIMER_SLICE);
        p.write_volatile(p.read_volatile().wrapping_add(1));
    }
}

pub fn pid() -> i32 {
    unsafe { core::ptr::read_volatile(core::ptr::addr_of!(CUR_TASK)) + 1 }
}

pub fn set_current(pid: i32) {
    unsafe { core::ptr::write_volatile(core::ptr::addr_of_mut!(CUR_TASK), pid - 1) }
}

pub fn alive(pid: i32) -> bool {
    if pid < 1 || pid > TASK_MAX as i32 {
        return false;
    }
    unsafe { (*task_at((pid - 1) as usize)).state == 1 }
}

pub fn input_owner() -> i32 {
    unsafe { core::ptr::read_volatile(core::ptr::addr_of!(INPUT_OWNER)) }
}

pub fn set_input_owner(pid: i32) {
    unsafe { core::ptr::write_volatile(core::ptr::addr_of_mut!(INPUT_OWNER), pid) }
}

pub fn kstack_top(pid: i32) -> u32 {
    unsafe { (*task_at((pid - 1) as usize)).kstack_top }
}

pub fn esp_of(pid: i32) -> u32 {
    unsafe { (*task_at((pid - 1) as usize)).esp }
}

pub fn jump_to(esp: u32) -> ! {
    unsafe { jump_to_task(esp) }
}
