// ===========================================================================
//  KERNEL k8: планировщик задач — вытесняющая многозадачность (циклическая)
// ===========================================================================
//  Как в ядре Linux (упрощённо): у каждой задачи СВОЙ стек ядра; процессор
//  переключается на него автоматически через TSS.esp0 при входе в ring 0.
//  Переключение делает таймер (IRQ0, каждый 2-й тик ≈ 20 мс): сохраняем
//  контекст текущей задачи и прыгаем в следующую (task_switch).
//
//  Новая задача стартует «из прерывания»: мы вручную собираем на её стеке
//  кадр, который выглядит как результат работы стаба (k7b), и адрес возврата
//  _irq_exit — первый же iretd отправляет её в ring 3 на заданный вход.
// ===========================================================================

struct task_t {
    unsigned int esp;          // сохранённый ESP ядра (0 = нет контекста)
    unsigned int kstack_top;   // вершина стека ядра (класть в TSS.esp0)
    int state;                 // 0 — свободна, 1 — жива, 2 — завершилась
    int parent;                // pid родителя (1-based), 0 — нет
    char name[16];
};

const int TASK_MAX = 4;
task_t tasks[TASK_MAX];
int task_count = 0;            // сколько слотов когда-либо занято (для pid)
int cur_task = 0;              // индекс текущей задачи (pid = индекс + 1)
int input_owner = 1;           // чей pid получает КЛАВИШИ (см. k6)

extern "C" void task_switch(unsigned int* save_esp, unsigned int new_esp);
extern "C" void jump_to_task(unsigned int new_esp);
extern "C" void irq_exit();

// --- Ассемблер переключения (callee-saved + подмена стека) -----------------
asm(
".text\n"
".global _task_switch\n"
"_task_switch:\n"
"    pushl %ebp\n"
"    pushl %ebx\n"
"    pushl %edi\n"
"    pushl %esi\n"
"    movl 20(%esp), %eax\n"        // save_esp (16 байт наших push + адрес возврата)
"    movl 24(%esp), %edx\n"        // new_esp
"    movl %esp, (%eax)\n"          // сохранили свой контекст
"    movl %edx, %esp\n"            // перешли на чужой
"    popl %esi\n"
"    popl %edi\n"
"    popl %ebx\n"
"    popl %ebp\n"
"    ret\n"
// --- первый запуск задачи из ядра (kernel_main -> ring 3): -----------------
// jump_to_task(esp) — «надеть» готовый контекст задачи и уйти в ring 3.
".global _jump_to_task\n"
"_jump_to_task:\n"
"    movl 4(%esp), %esp\n"
"    popl %esi\n"
"    popl %edi\n"
"    popl %ebx\n"
"    popl %ebp\n"
"    ret\n"
);

// Найти свободный слот задачи
int task_alloc() {
    for (int i = 0; i < TASK_MAX; i++)
        if (tasks[i].state != 1) return i;
    return -1;
}

// Стек ядра для задачи (4 КБ — хватает: ядро короткое, окна рисует оболочка)
unsigned int kstacks[TASK_MAX][1024] __attribute__((aligned(4096)));

// Создать задачу: entry — адрес кода (ring 3), ustack — вершина стека
// программы (ring 3). Возврат: pid (1..4) или -1.
int task_create(const char* name, unsigned int entry, unsigned int ustack, int parent) {
    int i = task_alloc();
    if (i < 0) return -1;
    unsigned int T = (unsigned int)(kstacks[i] + 1024);   // вершина стека ядра
    unsigned int* sp = (unsigned int*)T;

    // Собираем кадр СВЕРХУ ВНИЗ (адреса убывают). Порядок обязан совпадать
    // с k7b: [SS][UESP][EFLAGS][CS][EIP][ERR][VEC][DS][ES][FS][GS][pushal]
    // затем адрес возврата _irq_exit и «сохранённые» ESI/EDI/EBX/EBP.
    *(--sp) = 0x23;                 // SS  — сегмент данных программы
    *(--sp) = ustack;               // UESP программы
    *(--sp) = 0x202;                // EFLAGS (IF=1 — прерывания разрешены)
    *(--sp) = 0x1B;                 // CS  — сегмент кода программы (ring 3)
    *(--sp) = entry;                // EIP — вход программы
    *(--sp) = 0;                    // ERR
    *(--sp) = 32;                   // VEC (как будто тик таймера)
    for (int k = 0; k < 8; k++) *(--sp) = 0;     // pushal (EAX..EDI — мы вышли из iret)
    *(--sp) = 0x23;                 // DS
    *(--sp) = 0x23;                 // ES
    *(--sp) = 0x23;                 // FS
    *(--sp) = 0x23;                 // GS
    *(--sp) = (unsigned int)irq_exit;            // «возврат» стаба
    *(--sp) = 0;                    // EBP (task_switch pop-ит их в обратном порядке)
    *(--sp) = 0;                    // EBX
    *(--sp) = 0;                    // EDI
    *(--sp) = 0;                    // ESI
    tasks[i].esp = (unsigned int)sp;
    tasks[i].kstack_top = T;
    tasks[i].state = 1;
    tasks[i].parent = parent;
    int n = 0;
    while (n < 15 && name[n]) { tasks[i].name[n] = name[n]; n++; }
    tasks[i].name[n] = 0;
    if (i + 1 > task_count) task_count = i + 1;
    return i + 1;                   // pid = индекс + 1
}

// Переключиться на задачу next (если есть куда)
void switch_to(int next) {
    int old = cur_task;
    if (next == old || tasks[next].state != 1) return;
    cur_task = next;
    tss.esp0 = tasks[next].kstack_top;   // процессор возьмёт этот стек в ring 0
    task_switch(&tasks[old].esp, tasks[next].esp);
}

// Квант: каждые 2 тика (≈20 мс) — следующей живой задаче
void schedule_from_irq() {
    if (task_count < 2) return;
    if ((timer_slice & 1) == 0) return;
    for (int k = 1; k <= task_count; k++) {
        int j = (cur_task + k) % task_count;
        if (tasks[j].state == 1) { switch_to(j); return; }
    }
}

// Уступить процессор немедленно (SYS_YIELD, SYS_EXIT)
void schedule_now() {
    if (task_count < 2) return;
    for (int k = 1; k <= task_count; k++) {
        int j = (cur_task + k) % task_count;
        if (tasks[j].state == 1) { switch_to(j); return; }
    }
}

// Завершить задачу pid: ввод возвращается родителю, задача «убита».
void task_exit(int pid) {
    if (pid < 1 || pid > TASK_MAX) return;
    tasks[pid - 1].state = 2;
    if (input_owner == pid) input_owner = tasks[pid - 1].parent > 0 ?
                                          tasks[pid - 1].parent : 1;
    schedule_now();
    // Если живых больше нет — вечно спим (эту задачу уже не разбудят)
    for (;;) asm volatile("hlt");
}

// pid текущей задачи
int task_pid() { return cur_task + 1; }
bool task_alive(int pid) {
    if (pid < 1 || pid > TASK_MAX) return false;
    return tasks[pid - 1].state == 1;
}
