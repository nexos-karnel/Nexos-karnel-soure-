/* linker.ld — NexOS v0.3 ARM virt
 * RAM: 0x40000000, 128MB (QEMU default)
 */

ENTRY(_start)

MEMORY {
    RAM (rwx) : ORIGIN = 0x40000000, LENGTH = 128M
}

SECTIONS {
    /* ── TEXT ──────────────────────────────────────────── */
    .text 0x40000000 : {
        KEEP(*(.text.startup))   /* vector table + reset harus duluan */
        *(.text*)
        *(.rodata*)
    } > RAM

    . = ALIGN(8);

    /* ── DATA ──────────────────────────────────────────── */
    .data : {
        __data_start = .;
        *(.data*)
        __data_end = .;
    } > RAM

    . = ALIGN(8);

    /* ── BSS ───────────────────────────────────────────── */
    .bss : {
        __bss_start = .;
        *(.bss*)
        *(COMMON)
        __bss_end = .;
    } > RAM

    . = ALIGN(8);

    /* ── HEAP ──────────────────────────────────────────── */
    /* Heap mulai tepat setelah .bss — dibaca kernel via extern symbol */
    __heap_start = .;

    /* ── STACKS ────────────────────────────────────────── */
    /* IRQ mode stack — 4KB, di bawah main stack */
    __irq_stack_top  = ORIGIN(RAM) + LENGTH(RAM) - 8;
    /* SVC/main stack — tepat di bawah IRQ stack */
    __stack_top      = __irq_stack_top - 0x1000;

    /* Heap end = batas atas heap (bawah stack, margin 64KB) */
    __heap_end       = __stack_top - 0x10000;
}
