/* startup.s — NexOS ARM AArch32
 * Vector table + proper IRQ context save/restore
 * Target: ARM Cortex-A15, QEMU virt
 *
 * ARM AArch32 vector table layout (low vectors, 0x40000000):
 *   offset 0x00 — Reset
 *   offset 0x04 — Undefined Instruction
 *   offset 0x08 — SVC (Software Interrupt)
 *   offset 0x0C — Prefetch Abort
 *   offset 0x10 — Data Abort
 *   offset 0x14 — Reserved
 *   offset 0x18 — IRQ
 *   offset 0x1C — FIQ
 */

.section .text.startup
.global _start

/* ─── VECTOR TABLE ─────────────────────────────────────── */
/* Harus berada di awal .text — tepat di 0x40000000        */
/* Setiap entry = 1 instruksi branch (4 byte)              */
_start:
    ldr pc, =reset_handler           /* 0x00 Reset           */
    ldr pc, =undefined_handler       /* 0x04 Undefined       */
    ldr pc, =svc_handler             /* 0x08 SVC             */
    ldr pc, =prefetch_abort_handler  /* 0x0C Prefetch Abort  */
    ldr pc, =data_abort_handler      /* 0x10 Data Abort      */
    ldr pc, =.                       /* 0x14 Reserved (hang) */
    ldr pc, =irq_handler             /* 0x18 IRQ             */
    ldr pc, =fiq_handler             /* 0x1C FIQ             */

/* ─── RESET HANDLER ────────────────────────────────────── */
reset_handler:
    /* 1. Set stack untuk mode Supervisor (SVC) */
    /* CPSR mode bits: SVC = 0x13               */
    ldr     sp, =__stack_top

    /* 2. Setup IRQ mode stack (mode bits: IRQ = 0x12) */
    /* Masuk IRQ mode, disable IRQ+FIQ sementara       */
    msr     cpsr_c, #0xD2        /* IRQ mode, IRQ+FIQ disabled */
    ldr     sp, =__irq_stack_top /* stack khusus IRQ mode      */

    /* 3. Kembali ke SVC mode, IRQ masih disabled dulu */
    msr     cpsr_c, #0xD3        /* SVC mode, IRQ+FIQ disabled */

    /* 4. Zero BSS */
    ldr     r0, =__bss_start
    ldr     r1, =__bss_end
    mov     r2, #0
.bss_loop:
    cmp     r0, r1
    bge     .bss_done
    str     r2, [r0], #4
    b       .bss_loop
.bss_done:

    /* 5. Jump ke kernel_main — IRQ akan di-enable dari C */
    bl      kernel_main

    /* Fallback hang */
.hang:
    wfe
    b       .hang

/* ─── IRQ HANDLER ──────────────────────────────────────── */
/* PENTING: ARM IRQ masuk di IRQ mode, LR = PC+4 instruksi */
/* Harus save/restore semua register yang dipakai handler   */
irq_handler:
    /* Adjust LR: IRQ LR pointing ke instruksi SETELAH yang */
    /* diinterrupt, kita perlu return ke instruksi itu       */
    sub     lr, lr, #4

    /* Save context ke IRQ stack:                            */
    /* STMFD = push multiple, full descending stack          */
    /* ^ = save user mode registers (sp, lr dari user mode)  */
    stmfd   sp!, {r0-r12, lr}

    /* Panggil C IRQ handler */
    bl      irq_handler_c

    /* Restore context dan return dari IRQ                   */
    /* LDMFD dengan ^ dan pc = atomic restore + mode switch  */
    ldmfd   sp!, {r0-r12, pc}^

/* ─── STUB HANDLERS ────────────────────────────────────── */
/* Untuk sekarang: undefined/abort → hang + print error     */
undefined_handler:
    /* TODO: print "undefined instruction" via UART */
    b       .

svc_handler:
    b       .

prefetch_abort_handler:
    b       .

data_abort_handler:
    b       .

fiq_handler:
    b       .

