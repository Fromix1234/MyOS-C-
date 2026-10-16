// ===========================================================================
//  KERNEL k7b: PIC, IDT и ассемблерный «вход» в ядро (int 0x80, IRQ, ловушки)
// ===========================================================================
//  Как устроен вход (единый кадр для C — ровно как int_frame в дизайне):
//    CPU сам кладёт [EIP,CS,EFLAGS(,UESP,SS)]; наш стаб добавляет СВЕРХУ
//    (в порядке убывания адресов): err, vec, затем pushal, затем ds/es/fs/gs.
//    Тогда в памяти (от младшего адреса): EDI,ESI,EBP,ESP,EBX,EDX,ECX,EAX,
//    GS,FS,ES,DS,VEC,ERR,EIP,CS,EFLAGS,UESP,SS.
//    Отсюда: EAX=frame[7], EBX=frame[4], ECX=frame[6], EDX=frame[5],
//            ESI=frame[1], EDI=frame[0], VEC=frame[12], ERR=frame[13].
//  Новая задача (см. k8) получает ИСКУССТВЕННЫЙ кадр — и «просыпается»
//  прямо в user-режиме через общий хвост _irq_exit (iretd).
// ===========================================================================

// --- Ассемблерные стабы и хвост выхода -------------------------------------
asm(
".text\n"
// --- int 0x80: системный вызов из ring 3 ----------------------------------
".global _isr_syscall\n"
"_isr_syscall:\n"
"    pushl $0\n"                            // err = 0 (унификация кадра)
"    pushl $0x80\n"                         // vec = 0x80
"    pushal\n"
"    pushl %ds\n" "    pushl %es\n" "    pushl %fs\n" "    pushl %gs\n"
"    movw $0x10, %ax\n"                     // сегменты ядра
"    movw %ax, %ds\n" "    movw %ax, %es\n" "    movw %ax, %fs\n" "    movw %ax, %gs\n"
"    pushl %esp\n"                          // аргумент: кадр
"    call _syscall_dispatch\n"
"    addl $4, %esp\n"
"    jmp _irq_exit\n"
// --- IRQ0/IRQ1/IRQ12 -------------------------------------------------------
".global _isr_irq0\n"
"_isr_irq0:\n"
"    pushl $0\n" "    pushl $32\n" "    jmp _irq_common\n"
".global _isr_irq1\n"
"_isr_irq1:\n"
"    pushl $0\n" "    pushl $33\n" "    jmp _irq_common\n"
".global _isr_irq12\n"
"_isr_irq12:\n"
"    pushl $0\n" "    pushl $44\n"
"_irq_common:\n"
"    pushal\n"
"    pushl %ds\n" "    pushl %es\n" "    pushl %fs\n" "    pushl %gs\n"
"    movw $0x10, %ax\n"
"    movw %ax, %ds\n" "    movw %ax, %es\n" "    movw %ax, %fs\n" "    movw %ax, %gs\n"
"    pushl %esp\n"
"    call _irq_dispatch\n"
"    addl $4, %esp\n"
// --- общий хвост: сюда попадают и нормальные возвраты, и новые задачи -------
".global _irq_exit\n"
"_irq_exit:\n"
"    popl %gs\n" "    popl %fs\n" "    popl %es\n" "    popl %ds\n"
"    popal\n"
"    addl $8, %esp\n"                       // снять vec и err
"    iret\n"
// --- ловушки CPU: #DF/#GP/#PF (ошибка уже на стеке — пушим только вектор) --
".global _isr_df\n"
"_isr_df:\n" "    pushl $8\n" "    jmp _fault_common\n"
".global _isr_gp\n"
"_isr_gp:\n" "    pushl $13\n" "    jmp _fault_common\n"
".global _isr_pf\n"
"_isr_pf:\n" "    pushl $14\n"
"_fault_common:\n"
"    pushal\n"
"    pushl %ds\n" "    pushl %es\n" "    pushl %fs\n" "    pushl %gs\n"
"    movw $0x10, %ax\n"
"    movw %ax, %ds\n" "    movw %ax, %es\n" "    movw %ax, %fs\n" "    movw %ax, %gs\n"
"    pushl %esp\n"
"    call _cpu_fault\n"
"    addl $4, %esp\n"
"1:\n"
"    hlt\n"
"    jmp 1b\n"
);

extern "C" void isr_syscall();
extern "C" void isr_irq0(); extern "C" void isr_irq1(); extern "C" void isr_irq12();
extern "C" void isr_df(); extern "C" void isr_gp(); extern "C" void isr_pf();
extern "C" void irq_exit();

// --- PIC: переназначить прерывания на 32..47 и разрешить только 0/1/12 -----
void pic_init() {
    outb(0x20, 0x11); outb(0x80, 0);        // ICW1: каскад, ICW4 будет
    outb(0xA0, 0x11); outb(0x80, 0);
    outb(0x21, 0x20); outb(0x80, 0);        // ICW2: master -> вектор 32
    outb(0xA1, 0x28); outb(0x80, 0);        // ICW2: slave  -> вектор 40
    outb(0x21, 0x04); outb(0x80, 0);        // ICW3: slave на линии 2
    outb(0xA1, 0x02); outb(0x80, 0);
    outb(0x21, 0x01); outb(0x80, 0);        // ICW4: 8086-режим
    outb(0xA1, 0x01); outb(0x80, 0);
    outb(0x21, 0xF8);                       // маска: разрешены IRQ0, IRQ1, IRQ2
    outb(0xA1, 0xEF);                       // slave: разрешён только IRQ12 (линия 4)
}

// --- Обработчик прерываний: EOI СРАЗУ (чтобы не потерять следующие) --------
// extern "C" — его зовёт ассемблерный стаб выше (символ _irq_dispatch).
extern "C" void irq_dispatch(unsigned int* regs) {
    unsigned int vec = regs[12];
    if (vec == 32) {                                    // таймер
        outb(0x20, 0x20);
        timer_tick();
        // Переключаем задачи ТОЛЬКО если прерывание застало ring 3.
        // (В ядре IF=0, исключение — холостой цикл; ring 0 не вытесняем.)
        if ((regs[15] & 3) == 3) schedule_from_irq();
        return;
    }
    if (vec == 33) {                                    // клавиатура
        outb(0x20, 0x20);
        kbd_irq();
        return;
    }
    if (vec == 44) {                                    // мышь
        outb(0xA0, 0x20); outb(0x20, 0x20);
        mouse_irq();
        return;
    }
    outb(0x20, 0x20);
}

// --- IDT: сисколл (доступен ring 3), IRQ и ловушки -------------------------
void idt_init() {
    idt_set(0x80, (unsigned int)isr_syscall, 3);     // дверь из ring 3
    idt_set(8,  (unsigned int)isr_df, 0);
    idt_set(13, (unsigned int)isr_gp, 0);
    idt_set(14, (unsigned int)isr_pf, 0);
    idt_set(32, (unsigned int)isr_irq0,  0);         // таймер
    idt_set(33, (unsigned int)isr_irq1,  0);         // клавиатура
    idt_set(44, (unsigned int)isr_irq12, 0);         // мышь
    struct { unsigned short lim; unsigned int base; } __attribute__((packed)) idtr;
    idtr.lim = sizeof(idt_tab) - 1;
    idtr.base = (unsigned int)idt_tab;
    asm volatile("lidt %0" :: "m"(idtr));
}
