; ===========================================================================
;  user_demo.asm — ПЕРВАЯ ПРОГРАММА ПОЛЬЗОВАТЕЛЯ (ring 3)
; ===========================================================================
;  Как в Linux: это ОТДЕЛЬНЫЙ бинарник (собрал nasm), который ЯДРО копирует
;  в память программ (адрес 0x500000 — по договору myos.h зона demo.bin) и
;  запускает как задачу ring 3. Порты железа программе НЕДОСТУПНЫ: диск,
;  клавиатура, мышь и время — только через системные вызовы (int 0x80),
;  как и положено в настоящей ОС.
;
;  Рисовать в видеопамять можно напрямую: ядро отобразило её в адресное
;  пространство программы (как /dev/fb0 в Linux) — поэтому пиксели дешёвые,
;  а на каждый кадр не нужен ни один сисколл.
; ===========================================================================

        org 0x500000
        bits 32            ; программа 32-битная! (nasm по умолчанию собирает 16 бит)

%define SYS_EXIT  0        ; выйти обратно в ядро
%define SYS_KEY   1        ; прочитать сканкод клавиши (-1 — нет)
%define SYS_FB    5        ; где видеопамять (ядро заполнит структуру)
%define SYS_PRINT 6        ; напечатать строку в консоль ядра

; --- 1. Спрашиваем у ядра, где видеопамять (SYS_FB) ----------------------
start:
        mov eax, SYS_FB
        mov ebx, fb_info
        int 0x80

        cmp dword [fb_info + 12], 32          ; bpp должен быть 32
        jne bad_mode

        mov eax, SYS_PRINT                    ; «привет» в консоль ядра
        mov ebx, msg_hello
        int 0x80

        mov esi, [fb_info]                    ; ESI = адрес видеопамяти
        mov edi, [fb_info + 16]               ; EDI = pitch (байт в строке)
        xor ebx, ebx                          ; EBX = счётчик цвета

; --- 2. Главный цикл: пульсирующий квадрат + клавиши ---------------------
loop:
        mov eax, SYS_KEY
        int 0x80
        cmp eax, -1
        je  .draw
        cmp eax, 0x10                         ; Q — выход
        je  quit
        cmp eax, 0x01                         ; Esc — выход
        je  quit
        mov eax, SYS_PRINT                    ; любая другая — сообщить ядру
        mov ebx, msg_key
        int 0x80

.draw:
        add ebx, 0x00040101                   ; плавно меняем цвет
        and ebx, 0x00FFFFFF
        mov edx, 300                          ; Y от 300 до 339
        mov ebp, 40
.rows:
        mov eax, edx
        imul eax, edi                         ; Y * pitch
        add eax, esi                          ; + адрес видеопамяти
        lea eax, [eax + 400]                  ; + X = 100 (4 байта на пиксель)
        mov ecx, 200                          ; 200 пикселей в строке
.px:
        mov [eax], ebx
        add eax, 4
        dec ecx
        jnz .px
        inc edx
        dec ebp
        jnz .rows
        jmp loop

; --- 3. Выход: сказать «пока» и вернуться в ядро --------------------------
quit:
        mov eax, SYS_PRINT
        mov ebx, msg_bye
        int 0x80
        mov eax, SYS_EXIT
        int 0x80
        jmp $                                 ; сюда уже не вернёмся

bad_mode:
        mov eax, SYS_PRINT
        mov ebx, msg_bad
        int 0x80
        mov eax, SYS_EXIT
        int 0x80
        jmp $

; --- данные ----------------------------------------------------------------
msg_hello: db "user(ring3): hello! Drawing straight into video memory.", 10, 0
msg_key:   db "user(ring3): key pressed (Q or Esc exits)", 10, 0
msg_bye:   db "user(ring3): bye! returning to the kernel.", 10, 0
msg_bad:   db "user(ring3): video is not 32bpp - cannot draw.", 10, 0

        align 4
fb_info:   dd 0, 0, 0, 0, 0, 0, 0, 0        ; addr,w,h,bpp,pitch,rpos,gpos,bpos — ядро
