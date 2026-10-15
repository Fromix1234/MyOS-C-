; ===========================================================================
;  boot.asm — ЭТАП 1: MBR (главный загрузочный сектор диска, 512 байт)
; ===========================================================================
;  Задача: найти в таблице разделов АКТИВНЫЙ (флаг 0x80) и передать
;  управление загрузочному сектору этого раздела — VBR.
;
;  Цепочка загрузки целиком:
;    BIOS -> MBR (мы) -> VBR раздела -> stage2 -> ядро
;
;  VBR читаем в 0x7E00 и прыгаем туда (НЕ в 0x7C00, где сидим сами —
;  иначе затёрли бы собственный код прямо во время выполнения!).
;
;  Чтение диска — через BIOS int 13h (CHS). Переводим LBA в CHS по
;  геометрии диска (int 13h AH=08). Для флешек — повторы и сброс контроллера.
; ===========================================================================

[bits 16]
[org 0x7C00]

start:
    xor ax, ax
    mov ds, ax            ; DS = 0 (адресация наших переменных и строк)
    mov es, ax
    mov [boot_drive], dl  ; BIOS положил номер загрузочного диска в DL

    mov si, msg_mbr
    call print

    ; --- Ищем активный раздел в таблице (4 записи по 16 байт с 0x1BE) ---
    mov si, 0x7C00 + 446  ; первая запись таблицы разделов
    mov cx, 4
    mov byte [part_no], 1
.find:
    cmp byte [si], 0x80   ; флаг «активный»?
    jne .next
    cmp byte [si+4], 0    ; тип раздела не 0 (не пустой слот)?
    jne .found
.next:
    add si, 16
    inc byte [part_no]
    loop .find
    jmp .noactive         ; активного раздела нет — честно сообщаем

.found:
    mov ax, [si+8]        ; LBA начала раздела (младшие 16 бит)
    mov dx, [si+10]       ; старшие 16 бит
    or dx, dx
    jnz .biglba           ; диск больше 32 ГБ — пока не поддерживаем
    mov [vbr_lba], ax

    ; --- Печатаем «P<N> active» ---
    mov si, msg_p
    call print
    mov al, [part_no]
    add al, '0'
    mov ah, 0x0E
    int 0x10
    mov si, msg_active
    call print

    ; --- Геометрия диска (секторов на трек и голов) ---
    mov dl, [boot_drive]
    mov ah, 0x08
    int 0x13
    jc .geoerror
    mov al, cl
    and al, 0x3F          ; секторов на трек — младшие 6 бит CL
    xor ah, ah
    mov [spt_w], ax
    xor ax, ax
    mov al, dh            ; голов = макс. номер + 1
    inc ax
    mov [heads_w], ax

    ; --- LBA -> CHS и чтение VBR в 0x0000:0x7E00 ---
    mov ax, [vbr_lba]
    call lba_to_chs

    xor ax, ax
    mov es, ax            ; ES:BX = 0000:7E00
    mov bx, 0x7E00
    mov bp, 4             ; до 4 попыток
.read:
    mov dl, [boot_drive]
    xor ah, ah            ; сброс контроллера перед попыткой
    int 0x13

    mov dl, [boot_drive]
    mov ch, [cylin]       ; CHS из lba_to_chs
    mov cl, [secs]
    mov dh, [head]
    mov ah, 0x02          ; «прочитать секторы»
    mov al, 1             ; ровно 1 сектор
    int 0x13
    jnc .ok
    dec bp
    jnz .read
    jmp .diskerr

.ok:
    mov dl, [boot_drive]  ; VBR ждёт номер диска в DL
    jmp 0x0000:0x7E00     ; управление — загрузочному сектору раздела

; --- Обработчики ошибок (внутри секции start) ---
.noactive:
    mov si, msg_noact
    call print
    jmp halt
.biglba:
    mov si, msg_big
    call print
    jmp halt
.geoerror:
    mov si, msg_geo
    call print
    jmp halt
.diskerr:
    mov si, msg_disk
    call print
halt:
    hlt
    jmp halt

; --- LBA (AX) -> CHS в переменные [cylin],[secs],[head] (для int 13h AH=02) ---
lba_to_chs:
    xor dx, dx
    div word [spt_w]      ; AX = трек, DX = сектор-1
    inc dl
    mov [secs], dl
    xor dx, dx
    div word [heads_w]    ; AX = цилиндр, DX = головка
    mov [head], dl
    mov [cylin], al       ; младшие 8 бит цилиндра
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
part_no:    db 0
vbr_lba:    dw 0
spt_w:      dw 0
heads_w:    dw 0
cylin:      db 0
secs:       db 0
head:       db 0

msg_mbr:    db "MyOS MBR: ", 0
msg_p:      db "P", 0
msg_active: db " active", 13, 10, 0
msg_noact:  db "ERROR: no active partition!", 0
msg_big:    db "ERROR: LBA too big (16-bit only)!", 0
msg_geo:    db "ERROR: no disk geometry!", 0
msg_disk:   db "ERROR: disk read failed!", 0

times 446-($-$$) db 0     ; код и данные кончаются ДО таблицы разделов
times 510-($-$$) db 0     ; таблицу (байты 446..509) пишет build.ps1 в образ
dw 0xAA55
