! Disc extension takeover probe. The CD-block firmware decodes it to 0x09075800 after an accepted
! disc authentication, when the disc header's +0xf8 (FAD) and +0xfc (encoded length) name it.
!
! The loader installs VBR from +0x00, copies +0x44 to the CD block revision 0x0f0002ff, which
! command 01 returns in CR4's low byte, then calls the entry at +0x3c. The entry copies all 256 ROM
! vectors before interrupts can use the new VBR, then replaces only IRQ6. Task 6's resume word
! replaces its message-wait continuation. Both wrappers forward to the ROM after counting, so the
! firmware keeps its normal command and drive behavior.
!
! The revision byte reports the counters to the SH-2: its high nibble is task 6 messages modulo 16,
! its low nibble IRQ6 entries modulo 16. The counters themselves are full longwords in the part of
! the disc extension area above the decoded image and vector table.

    .equ    probe_counters, 0x09077000
    .equ    probe_vectors, 0x09077800
    .equ    revision_address, 0x0f0002ff

    .text
    .global probe_start
probe_start:
! Loader header, 0x48 bytes.
    .long   probe_vectors           ! +0x00 VBR
    .long   0                       ! +0x04 task 1 resume address
    .long   0                       ! +0x08 task 2
    .long   0                       ! +0x0c task 3
    .long   0                       ! +0x10 task 4
    .long   0                       ! +0x14 task 5
    .long   probe_task6             ! +0x18 task 6
    .fill   8,4,0                   ! +0x1c..+0x38 task 7..14 resume addresses, none
    .long   probe_entry             ! +0x3c entry
    .long   0                       ! +0x40 callback for a card extension load, none
    .byte   0xa0                    ! +0x44 CD block revision, nonzero to install
    .byte   0                       ! +0x45 compared with a card extension's byte +1
    .word   0

! Entry, called from 0x1ac6 with SR 0xf0 on task 7's stack, PR 0x1aca. Every register but r14 and
! r15 is free, PR and the stack must come back as they were. Interrupts are masked while the new
! vector table is filled from the ROM's table at address 0.
    .balign 4
    .global probe_entry
probe_entry:
    mov.l   entry_counters,r1
    mov     #0,r0
    mov.l   r0,@r1                  ! task 6 count
    mov.l   r0,@(4,r1)              ! IRQ6 count
    mov.l   entry_revision,r1
    mov     #0,r0
    mov.b   r0,@r1                  ! publish both zero nibbles
    stc     vbr,r5
    tst     r5,r5
    bt      probe_entry_return
    mov     #0,r6
    mov.w   entry_vector_bytes,r7

probe_entry_copy:
    mov.l   @r6+,r0
    mov.l   r0,@r5
    add     #4,r5
    cmp/hs  r7,r6
    bf      probe_entry_copy
    stc     vbr,r5
    mov.w   entry_irq6_offset,r0
    mov.l   entry_irq6,r1
    mov.l   r1,@(r0,r5)

probe_entry_return:
    rts
    nop

    .balign 4
entry_counters:
    .long   probe_counters
entry_revision:
    .long   revision_address
entry_irq6:
    .long   probe_irq6
entry_vector_bytes:
    .word   0x0400
entry_irq6_offset:
    .word   0x0118

! Task 6 resumes here instead of at 0x2798, after the kernel has taken its message into r11. Count
! it and update the high nibble, then continue at the ROM's only task 6 wait continuation. Task 6
! reads only r11 there, so r0..r3 are free.
    .balign 4
probe_task6:
    mov.l   task6_counters,r1
    mov.l   @r1,r2
    add     #1,r2
    mov.l   r2,@r1
    mov     #15,r3
    and     r3,r2
    shll2   r2
    shll2   r2
    mov.l   task6_revision,r1
    mov.b   @r1,r0
    and     r3,r0
    or      r2,r0
    mov.b   r0,@r1
    mov.l   task6_continue,r1
    jmp     @r1
    nop

    .balign 4
task6_counters:
    .long   probe_counters
task6_revision:
    .long   revision_address
task6_continue:
    .long   0x00002798

! IRQ6 enters through the probe's vector table. Count it and update the low nibble, then jump to
! the handler named by the ROM vector with every register and the stack as the interrupt left them.
! The JMP takes its target from r0 before the delay slot restores the interrupted r0.
    .balign 4
probe_irq6:
    mov.l   r0,@-r15
    mov.l   r1,@-r15
    mov.l   r2,@-r15
    mov.l   irq6_counters,r0
    mov.l   @(4,r0),r1
    add     #1,r1
    mov.l   r1,@(4,r0)
    mov     #15,r2
    and     r2,r1
    mov.l   irq6_revision,r0
    mov.b   @r0,r2
    mov     #-16,r0
    and     r0,r2
    or      r1,r2
    mov.l   irq6_revision,r0
    mov.b   r2,@r0
    mov.l   @r15+,r2
    mov.l   @r15+,r1
    mov.l   irq6_rom_vector,r0
    mov.l   @r0,r0
    jmp     @r0
    mov.l   @r15+,r0

    .balign 4
irq6_counters:
    .long   probe_counters
irq6_revision:
    .long   revision_address
irq6_rom_vector:
    .long   0x00000118
    .global probe_end
probe_end:
