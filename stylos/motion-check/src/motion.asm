; Retain the original scalar compositor for mixed blocks and row tails.
bits 32
org 0x9ce440
dd mask_row, gray_row, rgba_row, preview_has_focus
mask_row:
    test ebp,15
    jnz .pixel
    mov edx,[edi+8]
    sub edx,ebp
    cmp edx,16
    jl .pixel
    mov edx,[esp+0x10]
    movdqu xmm0,[edx]
    pxor xmm1,xmm1
    pcmpeqb xmm0,xmm1
    pmovmskb ecx,xmm0
    cmp ecx,65535
    jne .pixel
    add edx,16
    add eax,64
    add ebp,16
    mov [esp+0x10],edx
    mov [esp+0x14],ebp
    cmp ebp,[edi+8]
    jl mask_row
    jmp 0x639d3e
.pixel:
    mov cl,[eax+2]
    cmp cl,255
    jmp 0x639bb6

gray_row:
    test byte [esp+0x30],15
    jnz .byte
    mov edx,[edi+0x468]
    test edx,edx
    jz .byte
    mov edx,[esi+8]
    sub edx,[esp+0x30]
    cmp edx,16
    jl .byte
    movdqu xmm0,[eax+ebx]
    pxor xmm1,xmm1
    pcmpeqb xmm0,xmm1
    pmovmskb edx,xmm0
    cmp edx,65535
    jne .byte
    add ebx,16
    add dword [esp+0x30],16
    mov edx,[esp+0x30]
    cmp edx,[esi+8]
    jl gray_row
    jmp 0x636918
.byte:
    mov edx,[edi+0x468]
    jmp 0x6368b6

rgba_row:
    test bl,3
    jnz .pixel
    cmp dword [ebp+0x468],0
    je .pixel
    mov edx,[edi+8]
    sub edx,ebx
    cmp edx,4
    jl .pixel
    movdqu xmm0,[ecx+esi]
    psrld xmm0,24
    pxor xmm1,xmm1
    pcmpeqd xmm0,xmm1
    movmskps edx,xmm0
    cmp edx,15
    jne .pixel
    add esi,16
    add ebx,4
    cmp ebx,[edi+8]
    jl rgba_row
    jmp 0x6369d8
.pixel:
    add ecx,esi
    push ecx
    push esi
    mov ecx,ebp
    jmp 0x6369c8

; These calls use the application's existing ANSI Win32 imports. A render on
; a different GUI thread or outside the preview uses the original function.
preview_has_focus:
    call [dword 0x9cf384]      ; GetFocus
    test eax,eax
    jz .other
    sub esp,20
    mov edx,esp
    push 20
    push edx
    push eax
    call [dword 0x9cf43c]      ; GetClassNameA
    cmp eax,14
    jne .no
    cmp dword [esp],'Moti'
    jne .no
    cmp dword [esp+4],'onCh'
    jne .no
    cmp dword [esp+8],'eckW'
    jne .no
    cmp word [esp+12],'nd'
    jne .no
    add esp,20
    mov eax,1
    ret
.no:
    add esp,20
.other:
    xor eax,eax
    ret
