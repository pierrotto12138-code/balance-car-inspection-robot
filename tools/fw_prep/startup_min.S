/* 最小 GCC 启动文件（冒烟测试用）：仅验证 arm-none-eabi-gcc 工具链可构建 F405 镜像 */
    .syntax unified
    .cpu cortex-m4
    .fpu softvfp
    .thumb

    .global g_pfnVectors
    .global Reset_Handler

    .section .isr_vector,"a",%progbits
    .type g_pfnVectors, %object
g_pfnVectors:
    .word 0x20020000            /* 初始 SP（RAM 顶 128K） */
    .word Reset_Handler
    .word Default_Handler       /* NMI */
    .word Default_Handler       /* HardFault */
    .word Default_Handler       /* MemManage */
    .word Default_Handler       /* BusFault */
    .word Default_Handler       /* UsageFault */
    .word 0
    .word 0
    .word 0
    .word 0
    .word Default_Handler       /* SVC */
    .word Default_Handler       /* DebugMon */
    .word 0
    .word Default_Handler       /* PendSV */
    .word Default_Handler       /* SysTick */
    .size g_pfnVectors, .-g_pfnVectors

    .section .text.Reset_Handler,"ax",%progbits
    .type Reset_Handler, %function
Reset_Handler:
    ldr r0, =_sidata
    ldr r1, =_sdata
    ldr r2, =_edata
    movs r3, #0
CopyLoop:
    cmp r1, r2
    bcs CopyDone
    ldr r4, [r0, r3]
    str r4, [r1, r3]
    adds r3, r3, #4
    adds r1, r1, #4
    b CopyLoop
CopyDone:
    ldr r0, =_sbss
    ldr r1, =_ebss
    movs r3, #0
ZeroLoop:
    cmp r0, r1
    bcs ZeroDone
    str r3, [r0]
    adds r0, r0, #4
    b ZeroLoop
ZeroDone:
    bl main
Hang:
    b Hang

    .section .text.Default_Handler,"ax",%progbits
    .type Default_Handler, %function
Default_Handler:
    b Default_Handler
