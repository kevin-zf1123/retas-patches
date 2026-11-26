// Stylos 2.6.0 x86: ESI points to MSG, EDI to the pre-translate dispatcher.
.intel_syntax noprefix
.text
resume:
    .byte 0xe9
    .long STYLOS_RESUME_RVA - 5  // manager subtracts the allocated section RVA
entry:
    cmp dword ptr [esi+4], 0x100   // WM_KEYDOWN
    jne dispatch
    cmp dword ptr [esi+8], 0xe5    // VK_PROCESSKEY
    jne dispatch
    pushad
    sub esp, 64
    mov ebp, esp
    push 64
    push ebp
    push dword ptr [esi]
    call dword ptr [0x9cf43c]     // GetClassNameA
    test eax, eax
    jz done

    // Inline, position-independent class names. Leave text editing controls to the IME.
    call classes
    .asciz "CCelWindow","CStageWindow","CCompositeWindow","CCompositeScoreWindow"
    .asciz "CToolPalette","CPreviewer","CFilePreviewerPalette","CLayerPalette","DrawFrameWnd"
    .byte 0
classes:
    pop ebx
next_class:
    mov edx, ebp
    mov ecx, ebx
compare_char:
    mov al, byte ptr [ecx]
    cmp al, byte ptr [edx]
    jne skip_class
    test al, al
    jz check_modifiers
    inc ecx
    inc edx
    jmp compare_char
skip_class:
    cmp byte ptr [ecx], 0
    lea ecx, [ecx+1]
    jne skip_class
    mov ebx, ecx
    cmp byte ptr [ebx], 0
    jne next_class
    jmp done

check_modifiers:
    mov ebx, 0x10                 // VK_SHIFT, VK_CONTROL, VK_MENU
modifier:
    push ebx
    call dword ptr [0x9cf3b0]     // GetAsyncKeyState
    test ax, 0x8000
    jnz done
    inc ebx
    cmp ebx, 0x12
    jbe modifier
    movzx eax, byte ptr [esi+14]  // scan code in bits 16..23 of MSG.lParam
    push 1                        // MAPVK_VSC_TO_VK
    push eax
    call dword ptr [0x9cf470]     // MapVirtualKeyA
    cmp eax, 'A'
    jb done
    cmp eax, 'Z'
    ja done
    mov ebx, eax
    call dword ptr [0x9d00f4]     // rcwGetMainWnd (RETAS main frame)
    test eax, eax
    jz done
    mov [esi], eax
    mov [esi+8], ebx
done:
    add esp, 64
    popad
dispatch:
    mov edx, [edi]
    push esi
    mov ecx, edi
    call dword ptr [edx+4]       // original virtual method, result in EAX
    jmp resume
