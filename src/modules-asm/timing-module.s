! A module for the card firmware, ../card-firmwares/card-firmware-asm/card-firmware.s, that times
! fixed blocks of SH-1 instructions against ITU channel 4, so that a console and an emulator can be
! compared kind by kind. Its package is its image, linked into the module slot by
! timing-module.ld, with the CRC that ../../tools/package.py writes.
!
! The argument: bits 31..24 the operation, bits 23..8 the samples, bits 7..0 the kernel.
! - Operation 0 takes samples, 1 to 1024, of one kernel, 0 to 15. A sample reads TCNT4, calls the
!   kernel and reads TCNT4 again. The result: the shortest sample in the high 16 bits, and in the
!   low 16 the clean samples, those longer than the shortest by one count at most.
! - Operation 1: the result is the clean samples' sum, from the last operation 0.
! - Any other operation, or samples or a kernel out of range, fails with 7 in the result's high
!   byte.
!
! The clock. The firmware's startup leaves ITU4 counting φ/8 and running free to 0xffff, so a count
! is 8 cycles, and the samples' difference wraps safely as long as a sample stays under 65536
! counts. Interrupts stay enabled while a sample runs. One that lands in a sample makes it longer,
! and the clean samples leave it out, while a DMA cycle steal spread over every sample stays in.
!
! The kernels. Each is 256 instructions of one kind, straight, then rts. Kernel 0 is empty, so its
! samples are the frame that every other kernel's include: a kernel's cycles an instruction are its
! clean mean less kernel 0's, over 256.
! - 1 alu: add between registers.
! - 2 bra: bra to the next pair with nop in its slot, the two counted.
! - 3 bt: bt taken, past a nop it never runs. T is set before each sample.
! - 4 muls: muls.w back to back.
! - 5, 6: mov.w and mov.l loads with post-increment from the module's BSS in the CD buffer's RAM.
! - 7, 8: the same loads from 0x09000000, the RAM's first sector slots, far from the module.
! - 9, 10: mov.w and mov.l stores with pre-decrement into the BSS.
! - 11: mov.l load, then an add of the loaded register, the pair counted.
! - 12: mov.l loads from the stack in on-chip RAM.
! - 13: mov.l stores to the stack in on-chip RAM, of the PR word the pass pushed, back onto itself.
! - 14: mov.w loads of TCNT4, an on-chip register.
! - 15: mov.w loads of 0x0a000008, the interface chip's command flag enables (area 2).
! All code runs from the module slot, in the CD buffer's RAM.
!
! Passes. Pass 0 checks the argument. Each later pass takes up to 16 samples. The pass after the
! last sample works out the results.
!
! The frame is transform-module.s's: the same header, entry and job record, and that file's notes
! on modules and on SH-1 assembly apply here. No services, so no imports.

    .equ    import_count, 0
    .equ    stack_depth, 4                  ! PR, pushed by every pass after the first
    .equ    kernel_count, 16
    .equ    kernel_length, 256              ! the instructions a kernel times
    .equ    samples_most, 1024
    .equ    pass_samples, 16
    .equ    tcnt4, 0x05ffff36
    .equ    far_data, 0x09000000
    .equ    interface_register, 0x0a000008
    .equ    data_bytes, 1024

! The job, in the BSS after the image, by offset from its start.
    .equ    job_result_at, 0                ! the result word's address, from pass 0's r6
    .equ    job_kernel, 4
    .equ    job_count, 8                    ! the samples asked for
    .equ    job_done, 12                    ! the samples taken
    .equ    job_clean_sum, 16               ! from the last operation 0
    .equ    job_samples, 20                 ! a word a sample, in counts
    .equ    job_data, job_samples + 2 * samples_most
    .equ    job_size, job_data + data_bytes

! For the linker script's check that the BSS fits the slot.
    .global module_bss_bytes
    .set    module_bss_bytes, job_size

    .text
    .global module_start
module_start:
! The package header.
    .long   0x444f534d                      ! +0 "DOSM"
    .word   0                               ! +4
    .word   import_count                    ! +6
    .long   module_end - module_start       ! +8 the image's bytes
    .long   job_size                        ! +12 BSS
    .long   stack_depth                     ! +16
    .long   module_entry                    ! +20
    .long   module_imports                  ! +24
    .long   0                               ! +28

! No services, so the import table is empty.
module_imports:

! The entry, once per pass: r4 the pass, r5 the argument, r6 the result word's address.
module_entry:
    mov.l   job_address,r13
    tst     r4,r4
    bf      timing_pass
! Pass 0: the operation.
    mov.l   r6,@(job_result_at,r13)
    mov     r5,r0
    shlr16  r0
    shlr8   r0
    tst     r0,r0
    bt      timing_setup
    cmp/eq  #1,r0
    bf      timing_refused
! Operation 1: the clean sum, at once.
    mov.l   @(job_clean_sum,r13),r0
    mov.l   r0,@r6
    rts
    mov     #1,r0

! Operation 0: the kernel and the samples checked and kept.
timing_setup:
    extu.b  r5,r0
    mov     #kernel_count,r1
    cmp/hs  r1,r0
    bt      timing_refused
    mov.l   r0,@(job_kernel,r13)
    mov     r5,r0
    shlr8   r0
    extu.w  r0,r0
    tst     r0,r0
    bt      timing_refused
    mov.w   samples_word,r1
    cmp/hi  r1,r0
    bt      timing_refused
    mov.l   r0,@(job_count,r13)
    mov     #0,r0
    mov.l   r0,@(job_done,r13)
    rts
    nop

timing_refused:
    mov     #7,r0
    shll16  r0
    shll8   r0
    mov.l   r0,@r6
    rts
    mov     #2,r0

! A later pass: PR pushed, as the samples call the kernel. Every sample taken: the results.
timing_pass:
    sts.l   pr,@-r15
    mov.l   @(job_done,r13),r12
    mov.l   @(job_count,r13),r0
    cmp/hs  r0,r12
    bt      timing_results
! This pass's samples end before r7: 16 more, or the last.
    mov     r12,r7
    add     #pass_samples,r7
    cmp/hi  r0,r7
    bf      timing_kernel
    mov     r0,r7

! The kernel's entry in the table, 12 bytes: its address, then r8 and r9 as it starts.
timing_kernel:
    mov.l   @(job_kernel,r13),r0
    mov     r0,r1
    shll2   r1
    shll2   r0
    shll    r0
    add     r1,r0
    mov.l   kernel_table_address,r1
    add     r0,r1
    mov.l   @r1,r10
    mov.l   @(4,r1),r5
    mov.l   @(8,r1),r6
    mov.l   tcnt4_address,r11

! A sample. A kernel may change r1..r4, r8, T, MACH and MACL, nothing else. r1 starts as the PR word
! on the stack, which kernel 13 writes back onto itself, and T set, which kernel 3 branches on.
timing_sample:
    mov     r5,r8
    mov     r6,r9
    mov.l   @r15,r1
    sett
    mov.w   @r11,r0
    jsr     @r10
    nop
    mov.w   @r11,r2
    sub     r0,r2
    extu.w  r2,r2
    mov     r12,r0
    shll    r0
    mov     r13,r3
    add     #job_samples,r3
    mov.w   r2,@(r0,r3)
    add     #1,r12
    cmp/hs  r7,r12
    bf      timing_sample
    mov.l   r12,@(job_done,r13)
    lds.l   @r15+,pr
    rts
    mov     #0,r0

! The results, r0 the samples: the shortest in r1, then the clean samples' sum in r8 and their
! number in r9.
timing_results:
    mov     r13,r2
    add     #job_samples,r2
    mov     r2,r3
    mov     r0,r4
    mov     #-1,r1
    extu.w  r1,r1

timing_shortest:
    mov.w   @r3+,r5
    extu.w  r5,r5
    cmp/hs  r1,r5
    bt      timing_longer
    mov     r5,r1
timing_longer:
    add     #-1,r4
    tst     r4,r4
    bf      timing_shortest
    mov     r1,r6
    add     #1,r6
    mov     r2,r3
    mov     r0,r4
    mov     #0,r8
    mov     #0,r9

timing_clean:
    mov.w   @r3+,r5
    extu.w  r5,r5
    cmp/hi  r6,r5
    bt      timing_unclean
    add     r5,r8
    add     #1,r9
timing_unclean:
    add     #-1,r4
    tst     r4,r4
    bf      timing_clean
    mov.l   r8,@(job_clean_sum,r13)
    shll16  r1
    or      r9,r1
    mov.l   @(job_result_at,r13),r2
    mov.l   r1,@r2
    lds.l   @r15+,pr
    rts
    mov     #1,r0

samples_word:
    .word   samples_most
    .balign 4
job_address:
    .long   module_end
kernel_table_address:
    .long   kernel_table
tcnt4_address:
    .long   tcnt4

! The kernels by number: each one's address, then r8 and r9 as it starts.
kernel_table:
    .long   kernel_empty, 0, 0
    .long   kernel_alu, 0, 0
    .long   kernel_bra, 0, 0
    .long   kernel_bt, 0, 0
    .long   kernel_muls, 0, 0
    .long   kernel_load_word, module_end + job_data, 0
    .long   kernel_load_long, module_end + job_data, 0
    .long   kernel_load_word, far_data, 0
    .long   kernel_load_long, far_data, 0
    .long   kernel_store_word, module_end + job_data + 2 * kernel_length, 0
    .long   kernel_store_long, module_end + job_data + 4 * kernel_length, 0
    .long   kernel_load_use, module_end + job_data, 0
    .long   kernel_stack_load, 0, 0
    .long   kernel_stack_store, 0, 0
    .long   kernel_register_load, 0, tcnt4
    .long   kernel_register_load, 0, interface_register

    .balign 4
kernel_empty:
    rts
    nop

    .balign 4
kernel_alu:
    .rept   kernel_length / 2
    add     r1,r2
    add     r3,r4
    .endr
    rts
    nop

    .balign 4
kernel_bra:
    .rept   kernel_length / 2
    bra     1f
    nop
1:
    .endr
    rts
    nop

    .balign 4
kernel_bt:
    .rept   kernel_length
    bt      1f
    nop
1:
    .endr
    rts
    nop

    .balign 4
kernel_muls:
    .rept   kernel_length
    muls.w  r1,r2
    .endr
    rts
    nop

    .balign 4
kernel_load_word:
    .rept   kernel_length / 4
    mov.w   @r8+,r1
    mov.w   @r8+,r2
    mov.w   @r8+,r3
    mov.w   @r8+,r4
    .endr
    rts
    nop

    .balign 4
kernel_load_long:
    .rept   kernel_length / 4
    mov.l   @r8+,r1
    mov.l   @r8+,r2
    mov.l   @r8+,r3
    mov.l   @r8+,r4
    .endr
    rts
    nop

    .balign 4
kernel_store_word:
    .rept   kernel_length
    mov.w   r1,@-r8
    .endr
    rts
    nop

    .balign 4
kernel_store_long:
    .rept   kernel_length
    mov.l   r1,@-r8
    .endr
    rts
    nop

    .balign 4
kernel_load_use:
    .rept   kernel_length / 2
    mov.l   @r8,r1
    add     r1,r2
    .endr
    rts
    nop

    .balign 4
kernel_stack_load:
    .rept   kernel_length / 4
    mov.l   @r15,r2
    mov.l   @r15,r3
    mov.l   @r15,r4
    mov.l   @r15,r2
    .endr
    rts
    nop

    .balign 4
kernel_stack_store:
    .rept   kernel_length
    mov.l   r1,@r15
    .endr
    rts
    nop

    .balign 4
kernel_register_load:
    .rept   kernel_length / 4
    mov.w   @r9,r1
    mov.w   @r9,r2
    mov.w   @r9,r3
    mov.w   @r9,r4
    .endr
    rts
    nop

! The image's last longword, the CRC the package tool writes.
    .balign 4
    .long   0
    .global module_end
module_end:
