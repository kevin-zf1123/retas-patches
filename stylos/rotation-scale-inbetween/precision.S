/* Host-specific adapters for the three row constructors' common layout.
   Integer controls at 98/9c retain their ABI. Only the managed image has
   the extra percent/degree residuals at e0/e4. */
.intel_syntax noprefix
.text
.macro entry name
.global \name
.hidden \name
\name:
.endm

/* Freestanding aggregate copies emitted by Clang. */
entry memcpy
    push esi
    push edi
    mov edi, [esp+12]
    mov esi, [esp+16]
    mov ecx, [esp+20]
    mov eax, edi
    rep movsb
    pop edi
    pop esi
    ret

entry memset
    push edi
    mov edi, [esp+8]
    mov eax, [esp+12]
    mov ecx, [esp+16]
    rep stosb
    mov eax, [esp+8]
    pop edi
    ret

entry reset_precision
    mov dword ptr [esi+0xe0], 0
    mov dword ptr [esi+0xe4], 0
    mov dword ptr [esi+0x98], 100
    ret

/* Setters compare against their old value to decide whether to rebuild.
   Entering the displayed integer also clears an existing fraction. */
.macro setter name, field, fraction
entry \name
    cmp dword ptr [ecx+\fraction], 0
    mov dword ptr [ecx+\fraction], 0
    jne 1f
    cmp eax, [ecx+\field]
1:  ret
.endm
setter compare_scale, 0x98, 0xe0
setter compare_angle, 0x9c, 0xe4

.macro load_value name, row, field, fraction
entry \name
    fild dword ptr [\row+\field]
    fadd dword ptr [\row+\fraction]
    ret
.endm
load_value bitmap_scale, ebx, 0x98, 0xe0
load_value matrix_scale, esi, 0x98, 0xe0
load_value matrix_angle, esi, 0x9c, 0xe4

entry inverse_angle
    fild dword ptr [esi+0x9c]
    fadd dword ptr [esi+0xe4]
    fchs
    ret

entry test_rotation
    cmp dword ptr [ebx+0x9c], 0
    jne 1f
    cmp dword ptr [ebx+0xe4], 0
1:  ret

/* Entered in place of the virtual CreateRotationImage call. Preserve the
   stock integer path (including other virtual implementations). The caller
   has already pushed -integerAngle and the interpolation mode. */
entry rotate_bitmap
    mov ecx, edi
    cmp dword ptr [ebx+0xe4], 0
    je integer_sampler
    cmp dword ptr [esp+12], 0
    jne stock_sampler
    fild dword ptr [esp+8]
    fsub dword ptr [ebx+0xe4]
    sub esp, 8
    fstp qword ptr [esp]
    push esi
    push edi
    call fast_rotate
    add esp, 16
    test eax, eax
    jnz rotated
    mov ecx, edi
    mov edx, [edi]
stock_sampler:
    fild dword ptr [esp+8]
    fsub dword ptr [ebx+0xe4]
    fstp dword ptr [esp+8]
    mov edx, [edx+0x20]

/* The supported rcwCmn renderer converts its integer argument to double
   exactly once. Recreate its initial x86 frame and exception registration,
   loading an FP32 argument at that conversion. Both native sampling modes,
   allocation, exceptions and destruction then follow the original routine.
   fractional_rotation_available checks this build before setting a residue. */
    push ebp
    mov ebp, esp
    push -1
    push dword ptr [edx+6]
    mov eax, fs:[0]
    push eax
    mov fs:[0], esp
    sub esp, 0x124
    mov eax, [ebp+8]
    fld qword ptr [eax+0x18]
    push ebx
    mov ebx, [eax+0xc]
    fstp qword ptr [ebp-0x24]
    push esi
    mov [ebp-0x1c], ebx
    fild dword ptr [ebp-0x1c]
    push edi
    mov edi, ecx
    mov ecx, [eax+0x10]
    fst qword ptr [ebp-0x78]
    mov [ebp-0x18], ecx
    fstp qword ptr [ebp-0xd8]
    mov eax, [ebp+0x10]
    test eax, eax
    fild dword ptr [ebp-0x18]
    mov [ebp-0x10], esp
    mov [ebp-0xd0], edi
    fst qword ptr [ebp-0xb4]
    fld dword ptr [ebp+0xc]
    lea edx, [edx+0x5e]
    jmp edx
integer_sampler:
    jmp dword ptr [edx+0x20]
rotated:
    ret 12
