/*
 * STM32F405RGT6 链接脚本（GCC / arm-none-eabi）
 *   Flash 1 MB @ 0x08000000 ｜ SRAM 128 KB @ 0x20000000 ｜ CCM 64 KB @ 0x10000000
 *   起始地址必须是 0x08000000 —— 这样产出的 .hex/.bin 才能直接给
 *   FlyMcu（串口 ISP）或 ST-Link 烧录（决策 34）。
 *
 *   参考：厂商基线 Keil 工程用的是默认 0x08000000 起、RAM 在 0x20000000。
 */
ENTRY(Reset_Handler)

MEMORY
{
  FLASH  (rx)  : ORIGIN = 0x08000000, LENGTH = 1024K
  RAM    (rwx) : ORIGIN = 0x20000000, LENGTH = 128K
  CCMRAM (rwx) : ORIGIN = 0x10000000, LENGTH = 64K     /* 暂未使用（CCM 不能被 DMA 访问）*/
}

/* 栈顶：放在 SRAM 顶部 */
_estack = ORIGIN(RAM) + LENGTH(RAM);

_Min_Heap_Size  = 0x400;    /* 1 KB，给 newlib（printf）用 */
_Min_Stack_Size = 0x800;    /* 2 KB，主栈（中断上下文也在这上面） */

SECTIONS
{
  /* 向量表必须落在最前面 */
  .isr_vector :
  {
    . = ALIGN(4);
    KEEP(*(.isr_vector))
    . = ALIGN(4);
  } > FLASH

  .text :
  {
    . = ALIGN(4);
    *(.text)
    *(.text*)
    *(.glue_7)
    *(.glue_7t)
    *(.eh_frame)
    KEEP(*(.init))
    KEEP(*(.fini))
    . = ALIGN(4);
    _etext = .;
  } > FLASH

  .rodata :
  {
    . = ALIGN(4);
    *(.rodata)
    *(.rodata*)
    . = ALIGN(4);
  } > FLASH

  .ARM.extab : { *(.ARM.extab* .gnu.linkonce.armextab.*) } > FLASH

  .ARM : {
    __exidx_start = .;
    *(.ARM.exidx*)
    __exidx_end = .;
  } > FLASH

  .preinit_array :
  {
    PROVIDE_HIDDEN(__preinit_array_start = .);
    KEEP(*(.preinit_array*))
    PROVIDE_HIDDEN(__preinit_array_end = .);
  } > FLASH

  .init_array :
  {
    PROVIDE_HIDDEN(__init_array_start = .);
    KEEP(*(SORT(.init_array.*)))
    KEEP(*(.init_array*))
    PROVIDE_HIDDEN(__init_array_end = .);
  } > FLASH

  .fini_array :
  {
    PROVIDE_HIDDEN(__fini_array_start = .);
    KEEP(*(SORT(.fini_array.*)))
    KEEP(*(.fini_array*))
    PROVIDE_HIDDEN(__fini_array_end = .);
  } > FLASH

  /* .data 存放在 RAM，初值放在 Flash，启动时由 Reset_Handler 搬过去 */
  _sidata = LOADADDR(.data);

  .data :
  {
    . = ALIGN(4);
    _sdata = .;
    *(.data)
    *(.data*)
    . = ALIGN(4);
    _edata = .;
  } > RAM AT> FLASH

  . = ALIGN(4);
  .bss :
  {
    _sbss = .;
    __bss_start__ = _sbss;
    *(.bss)
    *(.bss*)
    *(COMMON)
    . = ALIGN(4);
    _ebss = .;
    __bss_end__ = _ebss;
  } > RAM

  /* 给 malloc 的堆 + 主栈；并给出 newlib 需要的 end/_end */
  ._user_heap_stack :
  {
    . = ALIGN(8);
    PROVIDE(end = .);
    PROVIDE(_end = .);
    . = . + _Min_Heap_Size;
    . = . + _Min_Stack_Size;
    . = ALIGN(8);
  } > RAM

  /* 注意：这里**不要**写 ST 模板里的
   *   /DISCARD/ : { libc.a(*) libm.a(*) libgcc.a(*) }
   * 那会把整个 C 库丢掉 —— printf / memcpy / 除法辅助函数全都会消失。 */

  .ARM.attributes 0 : { *(.ARM.attributes) }
}
