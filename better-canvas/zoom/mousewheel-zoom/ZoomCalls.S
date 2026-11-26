.text

# The application uses x86 thiscall: ECX holds the object and the callee pops its arguments.
.globl ApplyStylosZoom
ApplyStylosZoom:
    movl 4(%esp), %eax
    movl 8(%esp), %ecx
    pushl $0
    pushl $1
    pushl 24(%esp)
    pushl 24(%esp)
    call *%eax
    ret

.globl DisplayStylosZoom
DisplayStylosZoom:
    movl 4(%esp), %eax
    movl 8(%esp), %ecx
    pushl $0
    pushl 20(%esp)
    pushl 20(%esp)
    call *%eax
    ret

.globl GetStylosViewport
GetStylosViewport:
    movl 4(%esp), %eax
    movl 8(%esp), %ecx
    pushl $0
    pushl $0
    pushl 20(%esp)
    call *%eax
    ret

.globl MapStylosPoint
MapStylosPoint:
    movl 4(%esp), %eax
    movl 8(%esp), %ecx
    movl 16(%esp), %edx
    pushl 12(%edx)
    pushl 8(%edx)
    pushl 4(%edx)
    pushl 0(%edx)
    pushl 28(%esp)
    call *%eax
    ret

.globl ScrollStylosView
ScrollStylosView:
    movl 4(%esp), %eax
    movl 8(%esp), %ecx
    pushl $0
    pushl $0x30006
    pushl 24(%esp)
    pushl 24(%esp)
    call *%eax
    ret

