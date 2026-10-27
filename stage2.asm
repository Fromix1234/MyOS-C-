; ===========================================================================
;  stage2.asm — ЭТАП 2 (1024 байта = 2 сектора): видео VESA + загрузка ядра
; ===========================================================================
;  Вход: VBR загрузил нас в 0x8000 (16-битный режим).
;  «Записка» в памяти (её оставил VBR):
;    0x0500 — word: LBA раздела, из которого грузимся
;    0x0502 — word: сигнатура 0x4D59 ('MY') — проверка подлинности записки
;
;  Что делаем:
;    1. Читаем «записку» — узнаём свой раздел
;    2. Узнаём геометрию диска (int 13h AH=08)
;    3. Читаем ядро ПОСЕКТОРНО: LBA = part_base + 3, +4, ...
;       Каждый сектор адресуем через LBA->CHS — границы треков не страшны
;    4. Ставим видеорежим VESA VBE (универсальный графический драйвер)
;    5. GDT, защищённый режим, прыжок на kernel_main
;
;  Раскладка раздела: +0 VBR, +1..+2 stage2 (этот файл), +3.. ядро
;
;  «Записка» о видео для ядра (пишем после установки режима):
;    0x0504 — dword: адрес видеопамяти (linear framebuffer)
;    0x0508 — word:  байт в строке (pitch)
;    0x050A — word:  ширина
;    0x050C — word:  высота
;    0x050E — byte:  бит на пиксель (24 или 32)
;    0x0510 — word:  сигнатура 0x4D56 ('VM')
;    0x0512 — byte:  позиция битов красного в пикселе (0..31)
;    0x0513 — byte:  позиция битов зелёного
;    0x0514 — byte:  позиция битов синего
;  Позиции каналов у разных BIOS/карт разные (BGRX или RGBX) — ядро
;  собирает цвет по этим маскам, поэтому картинка верна ВЕЗДЕ.
;
;  Аргументы сборки (через nasm -d...): KERNEL_SECTORS, KERNEL_ENTRY
; ===========================================================================

[bits 16]
[org 0x8000]

; --- Значения по умолчанию (если не переданы при сборке) ---
%ifndef KERNEL_SECTORS
%define KERNEL_SECTORS 16
%endif
%ifndef KERNEL_ENTRY
%define KERNEL_ENTRY 0x10000
%endif
; Образ оболочки, положенный в конец boot.img для загрузки с ISO/флоппи.
; При загрузке с жёсткого диска (DL=0x80) этот блок не читается — ядро
; возьмёт shell.bin из MyFS, как раньше.
%ifndef SHELL_BLOB_LBA
%define SHELL_BLOB_LBA 2560
%endif
%ifndef SHELL_BLOB_SECTORS
%define SHELL_BLOB_SECTORS 216
%endif
%ifndef SHELL_BLOB_BYTES
%define SHELL_BLOB_BYTES 110592
%endif
%define PREF_COUNT 6          ; сколько разрешений в таблице предпочтений

stage2:
    mov [boot_drive], dl      ; номер диска от BIOS (через VBR)

    ; --- «Записка» от VBR: из какого раздела грузимся ---
    mov ax, [0x0502]
    cmp ax, 0x4D59
    jne .note_error           ; записки нет — грузимся не по нашей цепочке
    mov ax, [0x0500]
    mov [part_base], ax

    mov si, msg_load
    call print

    ; --- Геометрия диска ---
    mov dl, [boot_drive]
    mov ah, 0x08
    int 0x13
    jc .geo_error
    mov al, cl
    and al, 0x3F              ; секторов на трек
    xor ah, ah
    mov [spt_w], ax
    xor ax, ax
    mov al, dh                ; голов = макс. номер + 1
    inc ax
    mov [heads_w], ax

    ; --- Ядро: первый сектор по LBA part_base + 3 ---
    mov ax, [part_base]
    add ax, 3
    mov [cur_lba], ax
    mov word [remaining], KERNEL_SECTORS

    mov ax, 0x1000
    mov es, ax                ; ES:BX = 0x10000 — куда класть ядро
    xor bx, bx

.sector_loop:
    mov ax, [cur_lba]
    call lba_to_chs           ; CHS текущего сектора -> [cylin],[secs],[head]

    mov bp, 4                 ; до 4 попыток на каждый сектор
.read_try:
    mov dl, [boot_drive]
    xor ah, ah                ; сброс контроллера
    int 0x13

    mov dl, [boot_drive]
    mov ch, [cylin]
    mov cl, [secs]
    mov dh, [head]
    mov ah, 0x02              ; «прочитать секторы»
    mov al, 1                 ; ровно 1 сектор за раз (не пересекаем трек)
    int 0x13
    jnc .sector_ok
    dec bp
    jnz .read_try
    jmp .disk_error

.sector_ok:
    add bx, 512               ; сдвинуть адрес назначения на 512 байт
    jnc .no_seg               ; BX не переполнился — сегмент не меняем
    mov ax, es                ; BX прошёл через 0x0000 (=64 КБ):
    add ax, 0x1000            ; переносимся на следующий 64-КБ сегмент,
    mov es, ax                ; чтобы ядро грузилось и дальше 64 КБ
.no_seg:
    inc word [cur_lba]        ; следующий LBA
    dec word [remaining]
    jnz .sector_loop

.loaded:
    ; --- ISO/флоппи: заранее положить shell.bin в 0x30000 ------------------
    ; BIOS загрузил нас с эмулированного флоппи (DL=0x00) — значит, это
    ; El Torito ISO. Читаем образ оболочки из конца boot.img и оставляем
    ; ядру «записку» 0x0520, чтобы оно не трогало ATA/MyFS на CD-приводе.
    cmp byte [boot_drive], 0
    jne .no_preload
    mov ax, SHELL_BLOB_LBA
    mov [cur_lba], ax
    mov word [remaining], SHELL_BLOB_SECTORS
    mov ax, 0x3000
    mov es, ax
    xor bx, bx
.preload_loop:
    mov ax, [cur_lba]
    call lba_to_chs
    mov bp, 4
.preload_try:
    mov dl, [boot_drive]
    xor ah, ah
    int 0x13
    mov dl, [boot_drive]
    mov ch, [cylin]
    mov cl, [secs]
    mov dh, [head]
    mov ah, 0x02
    mov al, 1
    int 0x13
    jnc .preload_ok
    dec bp
    jnz .preload_try
    jmp .disk_error
.preload_ok:
    add bx, 512
    jnc .preload_no_seg
    mov ax, es
    add ax, 0x1000
    mov es, ax
.preload_no_seg:
    inc word [cur_lba]
    dec word [remaining]
    jnz .preload_loop
    ; «Записка»: magic, точный размер, адрес и номер загрузочного диска.
    mov dword [0x0520], 0x504C4443      ; 'CDLP'
    mov dword [0x0524], SHELL_BLOB_BYTES
    mov dword [0x0528], 0x00030000
    movzx eax, byte [boot_drive]
    mov [0x052C], eax
.no_preload:
    ; --- Видеорежим VESA (пока в реальном режиме — BIOS доступен) ---
    call vesa_set

    ; --- A20: без него всё выше 1 МБ заворачивается в младший мегабайт ---
    ; (VirtualBox после BIOS оставляет A20 выключенным; shell грузится
    ;  в 0x100000, поэтому без этого ядро затирает собственный код.)
    call enable_a20

    ; --- 32-битный защищённый режим ---
    cli                     ; запретить прерывания
    lgdt [gdt_descriptor]   ; загрузить GDT
    mov eax, cr0
    or  eax, 1              ; бит PE = 1 — включить защищённый режим
    mov cr0, eax
    jmp 0x08:protected_mode ; дальний прыжок — загрузить 32-битный CS

; --- Обработчики ошибок (внутри секции stage2) ---
.note_error:               ; «записки» от VBR нет
    mov si, msg_note
    call print
    jmp halt
.geo_error:                ; не удалось узнать геометрию диска
    mov si, msg_geo
    call print
    jmp halt
.disk_error:               ; диск не прочитался
    mov si, msg_err
    call print
halt:
    hlt
    jmp halt

; --- LBA (AX) -> CHS в переменные [cylin],[secs],[head] ---
lba_to_chs:
    xor dx, dx
    div word [spt_w]      ; AX = трек, DX = сектор-1
    inc dl
    mov [secs], dl
    xor dx, dx
    div word [heads_w]    ; AX = цилиндр, DX = головка
    mov [head], dl
    mov [cylin], al
    mov al, ah
    shl al, 6
    or [secs], al         ; старшие 2 бита цилиндра — в биты 7-6 сектора
    ret

; --- Печать строки (SI), заканчивается нулём ---
print:
    lodsb
    test al, al
    jz .done
    mov ah, 0x0E
    int 0x10
    jmp print
.done:
    ret

; --- Включить линию A20 (доступ к памяти выше 1 МБ) -----------------------
; Сначала быстрый способ через порт 0x92 (PS/2 System Control Port A).
; Если он недоступен — классическая последовательность через контроллер
; клавиатуры (0x64/0x60): команда 0xD1 (write output port), данные 0xDF.
enable_a20:
    in  al, 0x92
    test al, 0x02
    jnz .done
    or  al, 0x02
    and al, 0xFE            ; бит 0 = fast reset, его трогать нельзя
    out 0x92, al
.done:
    ret

; ===========================================================================
;  VESA VBE — универсальный видеодрайвер (настройка режима через BIOS)
; ===========================================================================
;  Как работает:
;    1. int 10h AX=4F00 — «какие режимы умеет видеокарта» (VbeInfoBlock
;       кладём в 0x9000)
;    2. Пробегаем список режимов; для каждого — AX=4F01 (ModeInfoBlock
;       кладём в 0x9200): берём только графический режим с линейным
;       буфером (LFB), глубиной 32 или 24 бита
;    3. Из подходящих выбираем лучший по ТАБЛИЦЕ ПРЕДПОЧТЕНИЙ (ниже)
;    4. int 10h AX=4F02 + бит 0x4000 — установить режим с LFB
;    5. Оставляем ядру «записку» о видео (0x0504...)
;
;  ВАЖНО: в таблице предпочтений первая строка — самый желанный режим.
;  Хочешь другой приоритет — переставь строки местами и пересобери.
vesa_set:
    ; --- 1. VbeInfoBlock в 0x9000 (ES:DI) ---
    xor ax, ax
    mov es, ax
    mov di, 0x9000
    mov ax, 0x4F00
    int 0x10
    cmp ax, 0x004F
    jne .fail

    ; --- 2. Указатель на список режимов (в блоке: +0x0E offset, +0x10 segment) ---
    mov ax, [0x900E]
    mov [ml_off], ax
    mov ax, [0x9010]
    mov [ml_seg], ax

    mov word [best_mode], 0xFFFF
    mov word [best_pri], 0xFFFF

.next:
    mov ax, [ml_seg]
    mov es, ax
    mov si, [ml_off]
    mov cx, [es:si]           ; номер очередного режима
    add word [ml_off], 2
    cmp cx, 0xFFFF            ; 0xFFFF = конец списка
    je .scan_done

    ; --- 3. ModeInfoBlock в 0x9200 ---
    xor ax, ax
    mov es, ax
    mov di, 0x9200
    mov ax, 0x4F01
    int 0x10
    cmp ax, 0x004F
    jne .next

    ; поддерживается? графический? есть LFB? (биты 0, 4, 7 атрибутов)
    mov ax, [0x9200]
    and ax, 0x0091
    cmp ax, 0x0091
    jne .next

    ; глубина цвета: только 32 или 24 бита
    mov al, [0x9219]
    mov [cur_bpp], al
    cmp al, 32
    je .bpp_ok
    cmp al, 24
    jne .next
.bpp_ok:
    ; --- 4. Подходит ли разрешение под таблицу предпочтений? ---
    mov ax, [0x9212]          ; ширина
    mov bx, [0x9214]          ; высота
    call pref_index           ; AX = приоритет (0xFFFF = не из списка)
    cmp ax, 0xFFFF
    je .next
    cmp ax, [best_pri]
    jae .next                 ; не лучше уже найденного — пропускаем
    mov [best_pri], ax
    mov [best_mode], cx
    ; запоминаем параметры (ModeInfoBlock перезапишется на след. витке)
    mov ax, [0x9210]
    mov [best_pitch], ax      ; байт в строке
    mov ax, [0x9212]
    mov [best_w], ax
    mov ax, [0x9214]
    mov [best_h], ax
    mov al, [0x9219]
    mov [best_bpp], al
    mov al, [0x9220]          ; RedFieldPosition — где биты красного
    mov [best_rpos], al
    mov al, [0x9222]          ; GreenFieldPosition
    mov [best_gpos], al
    mov al, [0x9224]          ; BlueFieldPosition
    mov [best_bpos], al
    mov eax, [0x9228]         ; PhysBasePtr — физический адрес видеопамяти
    mov [best_fb], eax
    jmp .next

.scan_done:
    mov cx, [best_mode]
    cmp cx, 0xFFFF
    je .fail                  ; ни один режим из списка не найден

    ; --- 5. Установить режим (+0x4000 = использовать линейный буфер) ---
    or cx, 0x4000
    mov bx, cx
    mov ax, 0x4F02
    int 0x10
    cmp ax, 0x004F
    jne .fail

    ; --- 6. «Записка» о видео для ядра ---
    mov eax, [best_fb]
    mov [0x0504], eax
    mov ax, [best_pitch]
    mov [0x0508], ax
    mov ax, [best_w]
    mov [0x050A], ax
    mov ax, [best_h]
    mov [0x050C], ax
    mov al, [best_bpp]
    mov [0x050E], al
    mov al, [best_rpos]       ; позиции каналов красного/зелёного/синего
    mov [0x0512], al          ; (сразу ЗА сигнатурой 'VM' в 0x0510!)
    mov al, [best_gpos]
    mov [0x0513], al
    mov al, [best_bpos]
    mov [0x0514], al
    mov word [0x0510], 0x4D56 ; 'VM' — записка настоящая
    ret

.fail:
    mov si, msg_vesa
    call print
    jmp halt

; --- Приоритет разрешения: AX=ширина, BX=высота -> AX=приоритет ---
; Приоритет = позиция в таблице *2 (+1 для 24 бит — 32 бита чуть лучше).
; 0xFFFF — такого разрешения в таблице нет.
pref_index:
    mov si, pref_table
    xor dx, dx
.next:
    cmp dx, PREF_COUNT
    je .none
    cmp ax, [si]
    jne .skip
    cmp bx, [si+2]
    je .found
.skip:
    add si, 4
    inc dx
    jmp .next
.found:
    shl dx, 1
    cmp byte [cur_bpp], 24
    jne .done
    inc dx
.done:
    mov ax, dx
    ret
.none:
    mov ax, 0xFFFF
    ret

; --- Таблица предпочтений: первая строка — самый желанный режим ---
pref_table:
    dw 1024, 768
    dw 1920, 1080
    dw 1280, 720
    dw 1280, 1024
    dw 800,  600
    dw 640,  480

; --- Данные ---
boot_drive: db 0
spt_w:      dw 0     ; секторов на трек
heads_w:    dw 0     ; количество голов
part_base:  dw 0     ; LBA нашего раздела (из «записки»)
cur_lba:    dw 0     ; какой сектор читаем сейчас
remaining:  dw 0     ; сколько секторов ядра осталось
cylin:      db 0
secs:       db 0
head:       db 0
ml_off:     dw 0     ; список режимов VBE: смещение
ml_seg:     dw 0     ; ...и сегмент
best_mode:  dw 0xFFFF ; лучший найденный режим
best_pri:   dw 0xFFFF ; его приоритет
best_pitch: dw 0
best_w:     dw 0
best_h:     dw 0
best_bpp:   db 0
best_rpos:  db 0    ; позиция битов красного в пикселе
best_gpos:  db 0    ; ...зелёного
best_bpos:  db 0    ; ...синего
best_fb:    dd 0
cur_bpp:    db 0     ; глубина текущего рассматриваемого режима

msg_load: db "Loading C++ kernel from partition...", 13, 10, 0
msg_vesa: db "ERROR: no VESA LFB mode found!", 0
msg_note: db "ERROR: no VBR boot note!", 0
msg_geo:  db "DISK GEOMETRY ERROR!", 0
msg_err:  db "KERNEL DISK READ ERROR!", 0

; ===========================================================================
;  GDT (таблица сегментов) — «паспорта» для кода и данных в 32-битном режиме
; ===========================================================================
align 8
gdt_start:
    dq 0                    ; нулевой дескриптор (обязательный)

    ; Кодовой сегмент (selector 0x08): база=0, лимит=4ГБ, исполняемый
    dw 0xFFFF, 0x0000
    db 0x00, 10011010b, 11001111b, 0x00

    ; Сегмент данных (selector 0x10): база=0, лимит=4ГБ, читаемый/записываемый
    dw 0xFFFF, 0x0000
    db 0x00, 10010010b, 11001111b, 0x00
gdt_end:

gdt_descriptor:
    dw gdt_end - gdt_start - 1   ; размер GDT - 1
    dd gdt_start                  ; адрес GDT (абсолютный, база org)

; ===========================================================================
;  32-битный режим — точка входа в C++ ядро
; ===========================================================================
[bits 32]
protected_mode:
    mov ax, 0x10           ; сегменты данных
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov esp, 0x90000       ; стек в свободной памяти

    ; Прыгаем на точку входа C++ ядра (kernel_main).
    ; KERNEL_ENTRY подставляется при сборке (адрес kernel_main в ядре).
    jmp KERNEL_ENTRY

times 1024-($-$$) db 0     ; ровно 1024 байта = 2 сектора (не мешать ядру)
