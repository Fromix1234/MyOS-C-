// ===========================================================================
//  KERNEL k7: page tables, GDT/TSS, IDT helpers, paging
// ===========================================================================
//  MyOS v2.0 module. Split mechanically from kernel.cpp v1.2
//  (git a81d4f4) and then reworked: see README section 11.

#define PTE_P    0x1                        // страница присутствует
#define PTE_RW   0x2                        // можно писать
#define PTE_USER 0x4                        // доступна ring 3
#define PTE_PS   0x80                       // большая страница — 4 МБ

#define USER_BASE 0x00400000u               // 4 МБ: сюда грузим программу
#define USER_SIZE 0x00400000u               // 4 МБ: сколько ей отдаём

// Каталог страниц: 1024 записи (по 4 МБ каждая). Выровнен на 4096 — так
// требует процессор для регистра CR3.
unsigned int page_dir[1024] __attribute__((aligned(4096)));

// TSS — «шпаргалка» процессора: отсюда он берёт стек ядра (ss0/esp0),
// когда программа из ring 3 входит в ядро (int 0x80). Поля обязаны идти
// строго по порядку, размер структуры — ровно 104 байта.
struct tss_t {
    unsigned int prev, esp0, ss0, esp1, ss1, esp2, ss2;
    unsigned int cr3, eip, eflags, eax, ecx, edx, ebx, esp, ebp, esi, edi;
    unsigned int es, cs, ss, ds, fs, gs, ldt;
    unsigned short trap, iomap;
} __attribute__((packed));

unsigned char gdt_tab[6 * 8] __attribute__((aligned(16)));      // 6 сегментов
unsigned char idt_tab[256 * 8] __attribute__((aligned(16)));    // 256 шлюзов
tss_t tss;
unsigned int sys_stack[512] __attribute__((aligned(16)));       // стек ядра для сисколлов
void mem_init(unsigned int fba) {
    for (int i = 0; i < 1024; i++) page_dir[i] = 0;
    page_dir[0] = 0x00000000 | PTE_P | PTE_RW | PTE_PS;             // 0..4 МБ — ядро (только ring 0)
    page_dir[1] = USER_BASE  | PTE_P | PTE_RW | PTE_PS | PTE_USER;  // 4..8 МБ — программы (ring 3)
    // Видеопамять (LFB) обычно лежит выше 4 МБ. Если VBE-записки нет,
    // init_video оставляет fb = 0xA0000; в этом случае base == 0 и старый
    // код затирал page_dir[0..2], снимая USER с окна программ 4..8 МБ.
    // Поэтому отображаем LFB только если он реально выше 8 МБ.
    unsigned int base = fba & 0xFFC00000u;
    if (base >= 0x00400000u) {
        for (int i = 0; i < 3; i++)          // картину видеопамяти открываем и программам
            page_dir[(base >> 22) + i] = (base + (unsigned int)i * 0x400000) | PTE_P | PTE_RW | PTE_PS | PTE_USER;
    }
    asm volatile("mov %%cr4, %%eax\n\tor $0x10, %%eax\n\tmov %%eax, %%cr4" ::: "eax");       // включить страницы 4 МБ (PSE)
    asm volatile("mov %0, %%cr3" :: "r"(page_dir) : "memory");                                // каталог страниц
    asm volatile("mov %%cr0, %%eax\n\tor $0x80000000, %%eax\n\tmov %%eax, %%cr0" ::: "eax"); // и сами страницы (PG)
}
void gdt_set(int i, unsigned int base, unsigned int limit, unsigned char access, unsigned char flags) {
    unsigned char* d = gdt_tab + i * 8;
    d[0] = (unsigned char)(limit & 0xFF);
    d[1] = (unsigned char)((limit >> 8) & 0xFF);
    d[2] = (unsigned char)(base & 0xFF);
    d[3] = (unsigned char)((base >> 8) & 0xFF);
    d[4] = (unsigned char)((base >> 16) & 0xFF);
    d[5] = access;
    d[6] = (unsigned char)(((limit >> 16) & 0x0F) | flags);
    d[7] = (unsigned char)((base >> 24) & 0xFF);
}

// GDT + TSS: сегменты ядра (0x08/0x10), программы (0x1B/0x23) и TSS (0x28)
void gdt_init() {
    for (int i = 0; i < 48; i++) gdt_tab[i] = 0;
    gdt_set(1, 0, 0xFFFFF, 0x9A, 0xCF);      // 0x08 — код ядра
    gdt_set(2, 0, 0xFFFFF, 0x92, 0xCF);      // 0x10 — данные ядра
    gdt_set(3, 0, 0xFFFFF, 0xFA, 0xCF);      // 0x1B — код программы (ring 3)
    gdt_set(4, 0, 0xFFFFF, 0xF2, 0xCF);      // 0x23 — данные программы
    for (unsigned int i = 0; i < sizeof(tss); i++) ((unsigned char*)&tss)[i] = 0;
    tss.ss0  = 0x10;                          // стек ядра — сегмент данных ядра
    tss.esp0 = (unsigned int)(sys_stack + 512);
    tss.iomap = (unsigned short)sizeof(tss);  // I/O-карты нет: порты программам закрыты
    gdt_set(5, (unsigned int)&tss, sizeof(tss) - 1, 0x89, 0x00);
    struct { unsigned short lim; unsigned int base; } __attribute__((packed)) gdtr;
    gdtr.lim = sizeof(gdt_tab) - 1;
    gdtr.base = (unsigned int)gdt_tab;
    asm volatile("lgdt %0" :: "m"(gdtr));
    asm volatile("ljmp $0x08, $1f\n1:");      // перезагрузить CS
    asm volatile("mov $0x10, %%ax\n\tmov %%ax, %%ds\n\tmov %%ax, %%es\n\tmov %%ax, %%fs\n\tmov %%ax, %%gs\n\tmov %%ax, %%ss" ::: "eax", "memory");
    asm volatile("mov $0x28, %%ax\n\tltr %%ax" ::: "eax");   // пусть процессор знает про TSS
}

// Шлюз в таблице прерываний. dpl=3 — шлюз доступен программам (int 0x80).
void idt_set(int n, unsigned int handler, unsigned char dpl) {
    unsigned char* d = idt_tab + n * 8;
    for (int i = 0; i < 8; i++) d[i] = 0;
    if (!handler) return;
    d[0] = (unsigned char)(handler & 0xFF);
    d[1] = (unsigned char)((handler >> 8) & 0xFF);
    d[2] = 0x08;                               // селектор кода ядра
    d[5] = (unsigned char)(0x8E | (dpl << 5)); // шлюз прерывания + уровень доступа
    d[6] = (unsigned char)((handler >> 16) & 0xFF);
    d[7] = (unsigned char)((handler >> 24) & 0xFF);
}

// Сам idt_init (шлюз int 0x80 + векторы IRQ + ловушки) живёт в k7b_irq.cpp:
// там же ассемблерные стабы, с которыми он обязан совпадать по раскладке.
