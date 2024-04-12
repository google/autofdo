SECTIONS {
  .rodata : {
    . = 0x1000;
    *(.rodata);
  }
  .text : {
    . = 0x10000;
    *(.text.foo);
    . = 0x20000;
    *(.text.bar);
    . = 0x30000;
    *(.text._start);
  }
}