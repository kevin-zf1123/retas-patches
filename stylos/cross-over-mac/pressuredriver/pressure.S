// Stylos 2.6.0 x86: ESI is the pen state; return one x87 value as in the original.
.intel_syntax noprefix
.text
resume:
    .byte 0xe9
    .long STYLOS_RESUME_RVA - 5
entry:
    cmp dword ptr [esi+0x18], 0
    je mouse
    fldz                          // WinTab context: no packet yet, zero pressure
    jmp resume
mouse:
    fld qword ptr [0x9d3260]      // retain the host's original mouse fallback
    jmp resume
