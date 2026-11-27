.globl _NativeTile
_NativeTile:
    movl 4(%esp), %eax
    movl 8(%esp), %ecx
    pushl 32(%esp)
    pushl 32(%esp)
    pushl 32(%esp)
    pushl 32(%esp)
    pushl 32(%esp)
    pushl 32(%esp)
    call *%eax
    ret

