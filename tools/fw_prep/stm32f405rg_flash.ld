/* 最小链接脚本（冒烟测试用）：STM32F405RGT6 = 1MB Flash / 128KB RAM(+64KB CCM) */
ENTRY(Reset_Handler)

MEMORY
{
  FLASH (rx)  : ORIGIN = 0x08000000, LENGTH = 1024K
  RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 128K
  CCMRAM(rwx) : ORIGIN = 0x10000000, LENGTH = 64K
}

_estack = ORIGIN(RAM) + LENGTH(RAM);
_Min_Heap_Size = 0x200;
_Min_Stack_Size = 0x400;

SECTIONS
{
  .isr_vector : { KEEP(*(.isr_vector)) } > FLASH
  .text : {
    *(.text) *(.text*) *(.rodata) *(.rodata*) *(.glue_7) *(.glue_7t)
    KEEP(*(.init)) KEEP(*(.fini))
    . = ALIGN(4);
    _etext = .;
  } > FLASH

  .ARM.extab : { *(.ARM.extab*) } > FLASH
  .ARM : { __exidx_start = .; *(.ARM.exidx*) __exidx_end = .; } > FLASH

  _sidata = LOADADDR(.data);

  .data : {
    . = ALIGN(4);
    _sdata = .;
    *(.data) *(.data*)
    . = ALIGN(4);
    _edata = .;
  } > RAM AT> FLASH

  .bss : {
    . = ALIGN(4);
    _sbss = .;
    *(.bss) *(.bss*) *(COMMON)
    . = ALIGN(4);
    _ebss = .;
  } > RAM

  ._user_heap_stack : {
    . = ALIGN(8);
    PROVIDE(end = .);
    PROVIDE(_end = .);
    . = . + _Min_Heap_Size;
    . = . + _Min_Stack_Size;
    . = ALIGN(8);
  } > RAM

  /DISCARD/ : { libc.a(*) libm.a(*) libgcc.a(*) }
}
