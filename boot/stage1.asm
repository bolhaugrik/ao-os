; AO-OS stage1 - 512 bajtos MBR bootszektor
;
; Feladata egyetlen dolog: a stage2-t (LBA 1..63) beolvassa a 0x8000 cimre
; es raugrik. Minden mas a stage2 dolga.
;
; Kepernyore irt jelzesek:
;   '1'  stage1 fut
;   'E'  a BIOS nem tud INT 13h LBA-bovitest (AH=41h)
;   'D'  lemezolvasasi hiba
;
; Memoria: 0x7C00 stage1, 0x8000..0xFDFF stage2, verem 0x7C00 alatt.

bits 16
org 0x7C00

STAGE2_LOAD     equ 0x8000
STAGE2_SECTORS  equ 63
CHUNK           equ 16          ; szektor / INT 13h hivas (USB-BIOS-barat)

start:
    cli
    jmp 0x0000:.norm            ; CS:IP normalizalasa (07C0:0000 -> 0000:7C00)
.norm:
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    sti
    mov [drive], dl

    mov al, '1'
    call putc

    ; INT 13h bovitesek ellenorzese
    mov ah, 0x41
    mov bx, 0x55AA
    mov dl, [drive]
    int 0x13
    jc  .err_ext
    cmp bx, 0xAA55
    jne .err_ext

    ; stage2 beolvasasa CHUNK szektoros darabokban
    mov word [dap_lba], 1
    mov word [dap_off], STAGE2_LOAD
    mov cx, STAGE2_SECTORS
.read:
    mov ax, cx
    cmp ax, CHUNK
    jbe .n
    mov ax, CHUNK
.n:
    mov [dap_count], ax
    push cx
    push ax
    mov si, dap
    mov ah, 0x42
    mov dl, [drive]
    int 0x13
    pop ax
    pop cx
    jc  .err_disk
    add [dap_lba], ax
    shl ax, 9                   ; * 512
    add [dap_off], ax
    sub cx, [dap_count]
    jnz .read

    mov dl, [drive]
    jmp 0x0000:STAGE2_LOAD

.err_ext:
    mov al, 'E'
    jmp .die
.err_disk:
    mov al, 'D'
.die:
    call putc
.hang:
    hlt
    jmp .hang

; AL = karakter, BIOS teletype
putc:
    mov ah, 0x0E
    mov bx, 0x0007
    int 0x10
    ret

drive:      db 0

align 4
dap:
    db 0x10, 0
dap_count:  dw 0
dap_off:    dw 0
dap_seg:    dw 0
dap_lba:    dq 0

; --- particios tabla (0x1BE) ---
times 0x1BE-($-$$) db 0
    db 0x80                     ; bootolhato
    db 0x00, 0x02, 0x00         ; CHS kezdet (nem hasznalt)
    db 0x7F                     ; tipus: sajat / kiserleti
    db 0xFE, 0xFF, 0xFF         ; CHS veg (nem hasznalt)
    dd 2048                     ; LBA kezdet (mkimage irja felul)
    dd 0                        ; meret szektorban (mkimage irja felul)
    times 48 db 0               ; tovabbi 3 bejegyzes ures
    dw 0xAA55
