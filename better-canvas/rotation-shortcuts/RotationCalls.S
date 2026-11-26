#ifdef __clang__
#define ApplyStylosRotation _ApplyStylosRotation
#define DisplayStylosRotation _DisplayStylosRotation
#define CallStylosCommand _CallStylosCommand
#define RegisterStylosShortcut _RegisterStylosShortcut
#define RefreshStylosShortcuts _RefreshStylosShortcuts
#endif
.text
.globl ApplyStylosRotation
ApplyStylosRotation:
    movl 4(%esp), %eax
    movl 8(%esp), %ecx
    pushl $0
    pushl $0
    pushl 20(%esp)
    pushl $1
    call *%eax
    ret

.globl DisplayStylosRotation
.globl CallStylosCommand
CallStylosCommand:
DisplayStylosRotation:
    movl 4(%esp), %eax
    movl 8(%esp), %ecx
    pushl 12(%esp)
    call *%eax
    ret

.globl RegisterStylosShortcut
RegisterStylosShortcut:
    movl 4(%esp), %eax
    movl 8(%esp), %ecx
    pushl 16(%esp)
    pushl 16(%esp)
    call *%eax
    ret

.globl RefreshStylosShortcuts
RefreshStylosShortcuts:
    movl 4(%esp), %eax
    movl 8(%esp), %ecx
    jmp *%eax
