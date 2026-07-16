/* Minimal C-SKY/XT804 startup for W800 RAM-loaded image.
 * Image body is loaded at 0x20004000. The first 0x100 bytes are a vector table.
 */
.section .vectors, "a", @progbits
.align 10
.globl __Vectors
.type __Vectors, @object
__Vectors:
    .long Reset_Handler
    .rept 63
    .long Default_Handler
    .endr
.size __Vectors, . - __Vectors

.section .text.startup, "ax", @progbits
.align 2
.globl Reset_Handler
.type Reset_Handler, %function
Reset_Handler:
    /* Disable interrupts / set PSR to a known value like SDK startup. */
    lrw     r0, 0xe0000200
    mtcr    r0, psr

    /* Stack at top of documented 288 KiB RAM: 0x20000000..0x20047fff. */
    lrw     r0, 0x20047ff0
    mov     sp, r0

    /* Clear .bss. */
    lrw     r1, __bss_start__
    lrw     r2, __bss_end__
    movi    r0, 0
1:
    cmpne   r1, r2
    bf      2f
    st.w    r0, (r1, 0)
    addi    r1, 4
    br      1b
2:
    jbsr    main
3:
    br      3b
.size Reset_Handler, . - Reset_Handler

.globl Default_Handler
.type Default_Handler, %function
Default_Handler:
    br Default_Handler
.size Default_Handler, . - Default_Handler
