; ===========================================================================
;  vbr.asm — ЭТАП 1.5: VBR (загрузочный сектор РАЗДЕЛА, 512 байт)
; ===========================================================================
;  MBR загружает этот сектор по адресу 0x7E00 и передаёт сюда управление.
;  Задача: прочитать stage2 (ДВА сектора: +1 и +2) из СВОЕГО раздела
;  и прыгнуть на него (в 0x8000).
;
;  VBR не знает, в каком разделе лежит, поэтому перед установкой в него
;  ВПЕЧАТЫВАЮТ LBA его раздела — 4 байта по смещению 504 (поле base_lba).
;  Кто впечатывает: build.ps1 (для P1) и команда install (для любого раздела).
;
;  «Записка» для следующих этапов — в низкой памяти:
;    0x0500 = LBA раздела (word)
;    0x0502 = сигнатура 0x4D59 ('MY') — «записка настоящая»
;  Её читает stage2, а потом и ядро (команда info покажет, откуда загрузились).
; ===========================================================================

[bits 16]
[org 0x7E00]              ; MBR загружает нас сюда

start:
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov [boot_drive], dl  ; номер диска (передал MBR через DL)

    mov ax, [base_lba]        ; LBA нашего раздела (младшие 16 бит)
    mov [0x0500], ax          ; записка: LBA раздела
    mov word [0x0502], 0x4D59 ; записка: сигнатура 'MY'

    mov si, msg_vbr
    call print

    ; --- Геометрия диска ---
    mov dl, [boot_drive]
    mov ah, 0x08
    int 0x13
    jc .geoerror
    mov al, cl
    and al, 0x3F
    xor ah, ah
    mov [spt_w], ax
    xor ax, ax
    mov al, dh
    inc ax
    mov [heads_w], ax

    ; --- Читаем stage2: ДВА сектора — каждый отдельным вызовом,
    ;     чтобы чтение не упёрлось в границу трека ---
    xor ax, ax
    mov es, ax            ; ES:BX = 0000:xxxx — куда читать

    mov ax, [base_lba]
    inc ax                ; сектор 1 stage2 = LBA раздела + 1
    mov bx, 0x8000
    call read_one

    mov ax, [base_lba]
    add ax, 2             ; сектор 2 stage2 = LBA раздела + 2
    mov bx, 0x8200
    call read_one

    mov dl, [boot_drive]  ; stage2 ждёт номер диска в DL
    jmp 0x0000:0x8000     ; управление — stage2

; --- Обработчики ошибок ---
.geoerror:
    mov si, msg_geo
    call print
halt:
    hlt
    jmp halt

; --- Читаем один сектор: LBA в AX, адрес назначения — уже в ES:BX ---
read_one:
    call lba_to_chs
    mov bp, 4
.try:
    mov dl, [boot_drive]
    xor ah, ah            ; сброс контроллера
    int 0x13

    mov dl, [boot_drive]
    mov ch, [cylin]
    mov cl, [secs]
    mov dh, [head]
    mov ah, 0x02
    mov al, 1
    int 0x13
    jnc .ok
    dec bp
    jnz .try
    mov si, msg_disk      ; диск так и не прочитался
    call print
    jmp halt
.ok:
    ret

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

; --- Данные ---
boot_drive: db 0
spt_w:      dw 0
heads_w:    dw 0
cylin:      db 0
secs:       db 0
head:       db 0

msg_vbr:    db "VBR: loading stage2...", 13, 10, 0
msg_geo:    db "ERROR: no disk geometry!", 0
msg_disk:   db "ERROR: VBR disk read failed!", 0

; --- Поле-закладка на фиксированном смещении 504: LBA раздела (впечатывается
;     установщиком). Смещение жёсткое — его знают install и build.ps1! ---
times 504-($-$$) db 0
base_lba: dd 0
times 510-($-$$) db 0
dw 0xAA55
