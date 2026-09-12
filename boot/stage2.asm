; AO-OS stage2 - real mode bootloader, 0x8000-ra toltve a stage1 altal
;
; Sorrend (minden hiba meg szoveges modban lathato):
;   2  indul, TSC mentes
;   A  A20 engedelyezve
;   U  unreal mode (4 GiB-es DS/ES limit)
;   M  E820 memoriaterkep lekerve
;   K  kernel beolvasva 1 MiB-re
;   R  ramdisk beolvasva 4 MiB-re (ha van)
;   V  VBE mod kivalasztva es beallitva  (ez utan nincs szoveg)
;   -> laptablak, long mode, ugras a kernelre
;
; Hibak (megall): 'a' A20, 'm' E820, 'd' lemez, 'v' nincs hasznalhato VBE mod
;
; Alacsony memoria hasznalat (mind az EBDA 0x9F400 alatt):
;   0x1000  bootinfo (72 bajt)          0x1200  E820 bejegyzesek (64 x 24)
;   0x2000  VbeInfoBlock (512)          0x2400  ModeInfoBlock (256)
;   0x7B00  verem teteje                0x8000  stage2
;   0x10000 lemez bounce-puffer (32 KiB) 0x70000 laptablak (32 KiB)

bits 16
org 0x8000

BOOTINFO        equ 0x1000
E820_BUF        equ 0x1200
E820_MAX        equ 64
VBE_INFO        equ 0x2000
MODE_INFO       equ 0x2400
STACK_TOP       equ 0x7B00
BOUNCE_SEG      equ 0x1000          ; 0x10000
BOUNCE_LIN      equ 0x10000
BOUNCE_SECTORS  equ 32              ; 16 KiB / INT 13h hivas (USB-BIOS-ok biztonsagos hatara)
PT_BASE         equ 0x70000
KERNEL_PADDR    equ 0x100000
RAMDISK_PADDR   equ 0x400000

; bootinfo mezo-eltolasok (kernel/include/bootinfo.h tukre)
BI_MAGIC        equ 0
BI_SIZE         equ 4
BI_TSC          equ 8
BI_E820_COUNT   equ 16
BI_E820_PADDR   equ 20
BI_FB_PADDR     equ 24
BI_FB_WIDTH     equ 28
BI_FB_HEIGHT    equ 32
BI_FB_PITCH     equ 36
BI_FB_BPP       equ 40
BI_FB_RPOS      equ 41
BI_FB_GPOS      equ 42
BI_FB_BPOS      equ 43
BI_VBE_MODE     equ 44
BI_KERNEL_PADDR equ 48
BI_KERNEL_SIZE  equ 52
BI_RAMDISK_PADDR equ 56
BI_RAMDISK_SIZE equ 60
BI_BOOT_DRIVE   equ 64
BI_TOTAL        equ 72

; ---------------------------------------------------------------------
; fejlec: a mkimage.py az 'AOS2' utani mezoket irja felul
; ---------------------------------------------------------------------
    jmp short main
    times 4-($-$$) db 0
hdr_magic:          db 'AOS2'
hdr_kernel_lba:     dd 64
hdr_kernel_sectors: dd 0
hdr_ramdisk_lba:    dd 0
hdr_ramdisk_sectors: dd 0

; ---------------------------------------------------------------------
main:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, STACK_TOP
    sti
    mov [boot_drive], dl

    call serial_init
    mov al, '2'
    call putc

    ; bootinfo alap
    mov di, BOOTINFO
    mov dword [di+BI_MAGIC], 'AOBI'
    mov dword [di+BI_SIZE], BI_TOTAL
    rdtsc
    mov [di+BI_TSC], eax
    mov [di+BI_TSC+4], edx
    mov al, [boot_drive]
    mov [di+BI_BOOT_DRIVE], al

    call enable_a20
    mov al, 'A'
    call putc

    call enter_unreal
    mov al, 'U'
    call putc

    call do_e820
    mov al, 'M'
    call putc

    ; kernel -> 1 MiB
    mov eax, [hdr_kernel_lba]
    mov ecx, [hdr_kernel_sectors]
    mov edi, KERNEL_PADDR
    call load_region
    mov di, BOOTINFO
    mov dword [di+BI_KERNEL_PADDR], KERNEL_PADDR
    mov eax, [hdr_kernel_sectors]
    shl eax, 9
    mov [di+BI_KERNEL_SIZE], eax
    mov al, 'K'
    call putc

    ; ramdisk -> 4 MiB (opcionalis)
    mov ecx, [hdr_ramdisk_sectors]
    test ecx, ecx
    jz .no_ramdisk
    mov eax, [hdr_ramdisk_lba]
    mov edi, RAMDISK_PADDR
    call load_region
    mov di, BOOTINFO
    mov dword [di+BI_RAMDISK_PADDR], RAMDISK_PADDR
    mov eax, [hdr_ramdisk_sectors]
    shl eax, 9
    mov [di+BI_RAMDISK_SIZE], eax
    mov al, 'R'
    call putc
.no_ramdisk:

    call setup_vbe                  ; ezutan grafikus mod, csak soros port
    mov al, 'V'
    call serial_putc

    call setup_paging
    mov al, 'P'
    call serial_putc
    jmp enter_long

; ---------------------------------------------------------------------
; hibakezeles
; ---------------------------------------------------------------------
fail:                               ; AL = hibakod
    call putc
.hang:
    hlt
    jmp .hang

; ---------------------------------------------------------------------
; kimenet: BIOS teletype + COM1
; ---------------------------------------------------------------------
putc:
    push ax
    push bx
    mov ah, 0x0E
    mov bx, 0x0007
    int 0x10
    pop bx
    pop ax
    ; folytatas a soros porton
serial_putc:
    push dx
    push ax
    mov dx, 0x3FD
.wait:
    in al, dx
    test al, 0x20
    jz .wait
    pop ax
    mov dx, 0x3F8
    out dx, al
    pop dx
    ret

serial_init:                        ; COM1 115200 8N1, QEMU-hoz; valodi HW-n nincs port, artalmatlan
    mov dx, 0x3F9
    xor al, al
    out dx, al                      ; IER = 0
    mov dx, 0x3FB
    mov al, 0x80
    out dx, al                      ; DLAB
    mov dx, 0x3F8
    mov al, 1
    out dx, al                      ; osztó lo = 1 (115200)
    mov dx, 0x3F9
    xor al, al
    out dx, al                      ; osztó hi
    mov dx, 0x3FB
    mov al, 0x03
    out dx, al                      ; 8N1
    mov dx, 0x3FA
    mov al, 0xC7
    out dx, al                      ; FIFO
    mov dx, 0x3FC
    mov al, 0x03
    out dx, al                      ; DTR|RTS
    ret

; ---------------------------------------------------------------------
; A20
; ---------------------------------------------------------------------
enable_a20:
    call test_a20
    jnz .ok
    mov ax, 0x2401                  ; BIOS
    int 0x15
    call test_a20
    jnz .ok
    in al, 0x92                     ; Fast A20
    test al, 2
    jnz .t
    or al, 2
    and al, 0xFE
    out 0x92, al
.t:
    call test_a20
    jnz .ok
    mov al, 'a'
    jmp fail
.ok:
    ret

; ZF=1 ha az A20 le van tiltva (0x0500 es 0x100500 ugyanaz a bajt)
test_a20:
    push ds
    push es
    push si
    push di
    xor ax, ax
    mov ds, ax
    not ax
    mov es, ax
    mov si, 0x0500
    mov di, 0x0510
    mov al, [ds:si]
    push ax
    mov al, [es:di]
    push ax
    mov byte [ds:si], 0x00
    mov byte [es:di], 0xFF
    mov al, [ds:si]
    cmp al, 0xFF
    pop bx
    mov [es:di], bl
    pop bx
    mov [ds:si], bl
    pop di
    pop si
    pop es
    pop ds
    ret

; ---------------------------------------------------------------------
; unreal mode: DS/ES limit 4 GiB, utana visszaterunk real modeba
; ---------------------------------------------------------------------
enter_unreal:
    cli
    push ds
    push es
    o32 lgdt [gdt32_desc]
    mov eax, cr0
    or al, 1
    mov cr0, eax
    jmp short $+2
    mov bx, 0x08
    mov ds, bx
    mov es, bx
    and al, 0xFE
    mov cr0, eax
    pop es
    pop ds
    sti
    ret

; ---------------------------------------------------------------------
; E820
; ---------------------------------------------------------------------
do_e820:
    mov di, E820_BUF
    xor ebx, ebx
    xor bp, bp
.loop:
    mov eax, 0xE820
    mov edx, 0x534D4150
    mov ecx, 24
    mov dword [di+20], 1
    int 0x15
    jc .end
    cmp eax, 0x534D4150
    jne .err
    inc bp
    add di, 24
    cmp bp, E820_MAX
    jae .done
    test ebx, ebx
    jnz .loop
.done:
    movzx eax, bp
    mov [BOOTINFO+BI_E820_COUNT], eax
    mov dword [BOOTINFO+BI_E820_PADDR], E820_BUF
    ret
.end:
    test bp, bp
    jnz .done
.err:
    mov al, 'm'
    jmp fail

; ---------------------------------------------------------------------
; load_region: EAX = LBA, ECX = szektorszam, EDI = cel fizikai cim
; bounce-pufferen (0x10000) at, 32 KiB-os darabokban, unreal modeban masolva
; ---------------------------------------------------------------------
load_region:
.loop:
    test ecx, ecx
    jz .done
    mov ebx, ecx
    cmp ebx, BOUNCE_SECTORS
    jbe .n
    mov ebx, BOUNCE_SECTORS
.n:
    mov [dap_count], bx
    mov [dap_lba], eax
    mov word [dap_off], 0
    mov word [dap_seg], BOUNCE_SEG
    mov byte [retries], 3
.try:
    push eax
    push ecx
    push edi
    push ebx
    mov si, dap
    mov ah, 0x42
    mov dl, [boot_drive]
    int 0x13
    pop ebx
    pop edi
    pop ecx
    pop eax
    jnc .copy
    dec byte [retries]
    jz .err
    push ax
    xor ah, ah                      ; lemez reset, ujra
    mov dl, [boot_drive]
    int 0x13
    pop ax
    jmp .try
.copy:
    push eax
    push ecx
    mov esi, BOUNCE_LIN
    mov ecx, ebx
    shl ecx, 7                      ; szektor * 512 / 4
    a32 rep movsd                   ; DS:ESI -> ES:EDI, mindketto 4 GiB limit
    pop ecx
    pop eax
    add eax, ebx
    sub ecx, ebx
    push ax
    mov al, '.'
    call serial_putc
    pop ax
    jmp .loop
.done:
    ret
.err:
    mov al, 'd'
    jmp fail

; ---------------------------------------------------------------------
; VBE: modlista bejarasa, jelolt-felbontasok sorrendjeben
; ---------------------------------------------------------------------
setup_vbe:
    mov di, VBE_INFO
    mov dword [di], 'VBE2'
    mov ax, 0x4F00
    int 0x10
    cmp ax, 0x004F
    jne .err

    mov si, cand_list
.next_cand:
    mov ax, [si]
    test ax, ax
    jz .err
    mov [want_w], ax
    mov ax, [si+2]
    mov [want_h], ax
    push si
    mov si, [VBE_INFO+14]           ; modlista far pointer
    mov fs, [VBE_INFO+16]
.next_mode:
    mov cx, [fs:si]
    cmp cx, 0xFFFF
    je .cand_done
    add si, 2
    push si
    push cx
    mov di, MODE_INFO
    mov ax, 0x4F01
    int 0x10
    pop cx
    pop si
    cmp ax, 0x004F
    jne .next_mode
    mov ax, [MODE_INFO+0]           ; attributumok: tamogatott|grafikus|LFB
    and ax, 0x91
    cmp ax, 0x91
    jne .next_mode
    cmp byte [MODE_INFO+25], 32     ; bpp
    jne .next_mode
    cmp byte [MODE_INFO+27], 6      ; direct color
    jne .next_mode
    mov ax, [MODE_INFO+18]
    cmp ax, [want_w]
    jne .next_mode
    mov ax, [MODE_INFO+20]
    cmp ax, [want_h]
    jne .next_mode
    pop si                          ; talalat, CX = mod
    jmp .set
.cand_done:
    pop si
    add si, 4
    jmp .next_cand

.set:
    mov di, BOOTINFO
    mov [di+BI_VBE_MODE], cx
    movzx eax, word [MODE_INFO+18]
    mov [di+BI_FB_WIDTH], eax
    movzx eax, word [MODE_INFO+20]
    mov [di+BI_FB_HEIGHT], eax
    movzx eax, word [MODE_INFO+16]  ; BytesPerScanLine
    cmp word [VBE_INFO+4], 0x0300   ; VBE 3: LinBytesPerScanLine, ha nem nulla
    jb .pitch_ok
    movzx ebx, word [MODE_INFO+50]
    test ebx, ebx
    jz .pitch_ok
    mov eax, ebx
.pitch_ok:
    mov [di+BI_FB_PITCH], eax
    mov eax, [MODE_INFO+40]         ; PhysBasePtr
    mov [di+BI_FB_PADDR], eax
    mov al, [MODE_INFO+25]
    mov [di+BI_FB_BPP], al
    mov al, [MODE_INFO+32]
    mov [di+BI_FB_RPOS], al
    mov al, [MODE_INFO+34]
    mov [di+BI_FB_GPOS], al
    mov al, [MODE_INFO+36]
    mov [di+BI_FB_BPOS], al

    mov bx, cx
    or bx, 0x4000                   ; linearis framebuffer
    mov ax, 0x4F02
    int 0x10
    cmp ax, 0x004F
    jne .err
    ret
.err:
    mov al, 'v'
    jmp fail

cand_list:
    dw 1366, 768
    dw 1360, 768
    dw 1280, 720
    dw 1024, 768
    dw 800, 600
    dw 0, 0
want_w: dw 0
want_h: dw 0

; ---------------------------------------------------------------------
; laptablak 0x70000-tol (32 KiB):
;   0x70000 PML4      [0] es [256] -> PDPT_lo, [511] -> PDPT_hi
;   0x71000 PDPT_lo   [0..3] -> PD0..PD3  (0..4 GiB identity, ill. direkt map)
;   0x72000 PD0..PD3  2048 x 2 MiB lap
;   0x76000 PDPT_hi   [510] -> PD0       (kernel -2 GiB -> fizikai 0)
; ---------------------------------------------------------------------
setup_paging:
    mov edi, PT_BASE
    xor eax, eax
    mov ecx, 0x8000/4
    a32 rep stosd

    mov edi, PT_BASE
    mov dword [edi], 0x71003
    mov dword [edi+256*8], 0x71003
    mov dword [edi+511*8], 0x76003

    mov edi, 0x71000
    mov dword [edi], 0x72003
    mov dword [edi+8], 0x73003
    mov dword [edi+16], 0x74003
    mov dword [edi+24], 0x75003

    mov edi, 0x76000
    mov dword [edi+510*8], 0x72003

    mov edi, 0x72000
    mov eax, 0x83                   ; present | write | 2 MiB
    mov ecx, 2048
.fill:
    mov [edi], eax
    mov dword [edi+4], 0
    add eax, 0x200000
    add edi, 8
    dec ecx
    jnz .fill
    ret

; ---------------------------------------------------------------------
; long mode
; ---------------------------------------------------------------------
enter_long:
    cli
    mov eax, cr4
    or eax, 1 << 5                  ; PAE
    mov cr4, eax
    mov eax, PT_BASE
    mov cr3, eax
    mov ecx, 0xC0000080             ; EFER
    rdmsr
    or eax, 1 << 8                  ; LME
    wrmsr
    o32 lgdt [gdt64_desc]
    mov eax, cr0
    or eax, 0x80000001              ; PG | PE
    mov cr0, eax
    jmp 0x08:long_entry

bits 64
long_entry:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax
    mov rsp, STACK_TOP
    mov edi, BOOTINFO               ; RDI = bootinfo fizikai cime
    mov rax, KERNEL_PADDR
    jmp rax

bits 16
; ---------------------------------------------------------------------
; adatok
; ---------------------------------------------------------------------
align 8
gdt32:
    dq 0
    dq 0x00CF92000000FFFF           ; adat, base 0, limit 4 GiB
gdt32_desc:
    dw 15
    dd gdt32

align 8
gdt64:
    dq 0
    dq 0x00AF9A000000FFFF           ; kod 64 bit
    dq 0x00CF92000000FFFF           ; adat
gdt64_desc:
    dw 23
    dd gdt64

boot_drive: db 0
retries:    db 0

align 4
dap:
    db 0x10, 0
dap_count:  dw 0
dap_off:    dw 0
dap_seg:    dw 0
dap_lba:    dq 0
