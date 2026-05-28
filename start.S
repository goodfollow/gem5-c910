.section .text
.global _start

_start:
    la sp, stack_top   # 初始化栈
    call main

loop:
    j loop

.section .bss
.space 4096
stack_top:
