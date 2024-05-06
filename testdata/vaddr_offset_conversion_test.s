.section .text.foo,"ax",@progbits
.globl foo
.type foo,@function
foo:
.space 0x1000
.size foo,.-foo

.section .text.bar,"ax",@progbits
.globl bar
.type bar,@function
bar:
.space 0x100
.size bar,.-bar

.section .text._start,"ax",@progbits
.globl _start
.type _start,@function
_start:
.space 0x10
.size _start,.-_start

.section .rodata,"",@progbits
.globl hello
hello:
.long 42
.size hello,.-hello

.section .note.GNU-stack,"",@progbits