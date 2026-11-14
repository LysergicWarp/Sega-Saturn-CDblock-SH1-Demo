! A module for the card firmware, ../card-firmwares/card-firmware-asm/card-firmware.s, that
! transforms vertices by a fixed-point matrix, one input sector into one output sector per pass. Its
! package is its image, linked into the module slot by transform-module.ld, with the CRC that
! ../../tools/package.py writes.
!
! The argument: CR3's high byte the input partition and its low byte the output filter, both up to
! 23, and CR4 the most output sectors, from 1. The result is the sectors filed.
!
! - Each input sector, 2048 bytes from where its mode puts its data: a 3x4 matrix of int16 rows
!   (m0, m1, m2, t), the m in Q12, padded to 32 bytes, then 252 vectors of four int16 (x, y, z, w).
! - Each output sector keeps the 32 bytes and gives, per vector and row, the low 16 bits of
!   ((m0 x + m1 y + m2 z) >> 12) + t from a 32-bit sum, and w copied. It reads as a mode 0 sector,
!   so 61 with get sector length 0 sends its 2048 bytes.
! - A failed job's result is the reason in the high byte and the sectors filed below it. Reasons: 1
!   no input sector, 4 more input sectors than the limit, 5 no free slot, 6 a mode 2 form 2 input
!   sector, 7 an argument out of range.
! - The input stays in its partition, and the sectors filed stay in the filter's, whether the job is
!   done or failed. The host leaves both partitions alone until MPED, then deletes them.
!
! Reading this file. It is the shortest of the three modules, so the notes a reader new to the SH-1
! needs are here, and the other two refer to them.
!
! What a module is. The card firmware keeps one slot of the SH-1's RAM for a small program, loads
! a package like this one into it and calls its entry. The SH-1 is busy: it runs the drive and
! answers the Saturn's commands. So a job does not run to its end in one call. The firmware calls
! the entry once per pass, and the module does one bounded step, here one sector, and returns. What
! must last from one pass to the next is kept in the job's record in RAM, not in registers.
!
! The services. A module cannot touch the CD buffer by itself, since the buffer's slots belong to
! Sega's firmware. The card firmware offers five services for that, and the module lists those it
! uses by number in its import table. The load replaces each number with the service's address, so
! a call is: load the address from the table, jsr through it.
!
! The assembly, in the terms the code uses:
! - Registers. r0 to r7 are scratch, with r4 to r7 a call's arguments and r0 its result. r8 to r13
!   keep their values across a service call. r15 is the stack pointer. PR holds the return address
!   of the last call, so a routine that calls another saves PR on the stack first.
! - Every instruction is 16 bits, too few to hold a 32-bit constant. A constant such as an address
!   sits in a literal pool after the code, and "mov.l job_address,r13" loads it from there.
! - The T bit is the one condition flag. A compare or a test sets it, and bt and bf branch on it
!   true or false.
! - The delay slot. The instruction written after a jsr, bra or rts runs before the jump lands. So
!   "bra transform_fail" followed by "mov #7,r4" sets r4 and then jumps, and a nop fills the slot
!   where nothing useful fits. bt and bf have no delay slot.
! - The .equ lines give names to offsets, the way a struct gives names to its fields:
!   @(job_filter,r13) is the job record's filter field, with r13 the record's address.
!
! The arithmetic. The SH-1 has no floating point, so the matrix is in fixed point: Q12 means a
! value is stored multiplied by 4096, and 1.0 is 4096. A product of a Q12 factor and a coordinate
! is then 4096 times too large, which the shift right by 12 undoes. The SH-1 multiplies 16 bits by
! 16 only: muls.w puts the signed 32-bit product in the register MACL, which sts copies out.

    .equ    import_count, 4
    .equ    stack_depth, 4                  ! PR at the entry

! The services, by offset in the import table once the load has bound it.
    .equ    slot_at, 0
    .equ    sector_data, 4
    .equ    take, 8
    .equ    file, 12

! The job, in the BSS after the image, by offset from its start.
    .equ    job_result_at, 0                ! the result word's address, from the entry's r6
    .equ    job_partition, 4                ! the input partition
    .equ    job_filter, 8                   ! the output filter
    .equ    job_limit, 12                   ! the most output sectors
    .equ    job_position, 16                ! this pass's input position, the sectors filed so far
    .equ    job_slot, 20                    ! the pass's output slot
    .equ    job_size, 24

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

! The services by number. The load replaces each with its address.
module_imports:
    .long   1                               ! slot_at
    .long   2                               ! sector_data
    .long   3                               ! take
    .long   4                               ! file

! The entry, once per pass: r4 the pass, r5 the argument, r6 the result word's address. Pass n
! transforms the input sector at position n, and the pass that finds none ends the job.
!
! PR goes on the stack first, as the entry calls services, and each call overwrites PR. The job
! record is the module's BSS, the zeroed RAM right after its image, so its address is module_end.
module_entry:
    sts.l   pr,@-r15
    mov.l   job_address,r13
    mov.l   r6,@(job_result_at,r13)
    mov.l   r4,@(job_position,r13)
    tst     r4,r4
    bf      transform_sector
! The job's start: the argument checked and kept.
    mov     r5,r0
    shlr16  r0
    mov     r0,r1
    shlr8   r1
    extu.b  r0,r0
    mov     #24,r2
    cmp/hs  r2,r1
    bt      transform_argument_refused
    cmp/hs  r2,r0
    bt      transform_argument_refused
    mov.l   r0,@(job_filter,r13)
    mov.l   r1,@(job_partition,r13)
    extu.w  r5,r0
    tst     r0,r0
    bt      transform_argument_refused
    mov.l   r0,@(job_limit,r13)

! The input sector at this pass's position, with r8 its data. None left ends the job, with none at
! all reason 1.
!
! slot_at answers with a slot's number, or -1. A slot is one of the CD buffer's 200 places for a
! sector, and sector_data turns its number into the address of the sector's 2048 bytes. A sector
! from the disc has a header before them whose length depends on its mode, which is why the module
! asks instead of adding a fixed offset.
transform_sector:
    mov.l   @(job_partition,r13),r4
    mov.l   @(job_position,r13),r5
    mov.l   imports_address,r1
    mov.l   @(slot_at,r1),r0
    jsr     @r0
    nop
    cmp/pz  r0
    bt      transform_input
    mov.l   @(job_position,r13),r4
    tst     r4,r4
    bt      transform_no_input
    bra     transform_end
    mov     #1,r5

transform_input:
    mov     r0,r4
    mov.l   @(job_position,r13),r0
    mov.l   @(job_limit,r13),r1
    cmp/hs  r1,r0
    bt      transform_over_limit
    mov.l   imports_address,r1
    mov.l   @(sector_data,r1),r0
    jsr     @r0
    nop
    tst     r0,r0
    bt      transform_form2
    mov     r0,r8
! A free slot for the output, with r9 its data. No free slot: reason 5.
    mov.l   imports_address,r1
    mov.l   @(take,r1),r0
    jsr     @r0
    nop
    cmp/eq  #-1,r0
    bt      transform_no_slot
    mov.l   r0,@(job_slot,r13)
    mov     r0,r4
    mov.l   imports_address,r1
    mov.l   @(sector_data,r1),r0
    jsr     @r0
    nop
    mov     r0,r9

! The transform, from r8 to r9: r5 walks the input, r6 the output and r4 the matrix rows.
    mov     r8,r5
    mov     r9,r6
    mov     #8,r7

transform_header:
    mov.l   @r5+,r0
    mov.l   r0,@r6
    add     #4,r6
    add     #-1,r7
    tst     r7,r7
    bf      transform_header
    mov.w   vector_count,r11

transform_vector:
    mov.w   @r5+,r1
    mov.w   @r5+,r2
    mov.w   @r5+,r3
    mov     r8,r4
    mov     #3,r7

transform_row:
    mov.w   @r4+,r0
    muls.w  r0,r1
    sts     macl,r12
    mov.w   @r4+,r0
    muls.w  r0,r2
    sts     macl,r0
    add     r0,r12
    mov.w   @r4+,r0
    muls.w  r0,r3
    sts     macl,r0
    add     r0,r12
! Bits 12..27 of the sum are the low 16 bits of the sum >> 12, so a logical shift serves.
    shlr8   r12
    shlr2   r12
    shlr2   r12
    mov.w   @r4+,r0
    add     r0,r12
    mov.w   r12,@r6
    add     #2,r6
    add     #-1,r7
    tst     r7,r7
    bf      transform_row
    mov.w   @r5+,r0
    mov.w   r0,@r6
    add     #2,r6
    add     #-1,r11
    tst     r11,r11
    bf      transform_vector

! Filed through the job's filter. A job that is no longer its own has had the slot returned, and
! ends with nothing published.
!
! Filing is what makes the sector the Saturn's to read: until then the slot is only the module's
! scratch. Returning 0 from here asks the firmware for another pass.
    mov.l   @(job_slot,r13),r4
    mov.l   @(job_filter,r13),r5
    mov.l   imports_address,r1
    mov.l   @(file,r1),r0
    jsr     @r0
    nop
    tst     r0,r0
    bt      transform_dropped
    mov     #0,r0
    lds.l   @r15+,pr
    rts
    nop

transform_argument_refused:
    mov     #0,r0
    mov.l   r0,@(job_position,r13)
    bra     transform_fail
    mov     #7,r4

transform_no_input:
    bra     transform_fail
    mov     #1,r4

transform_over_limit:
    bra     transform_fail
    mov     #4,r4

transform_form2:
    bra     transform_fail
    mov     #6,r4

transform_no_slot:
    bra     transform_fail
    mov     #5,r4

transform_dropped:
    bra     transform_fail
    mov     #0,r4

! A failed job, r4 the reason: the result is the reason in its high byte and the sectors filed below.
transform_fail:
    shll16  r4
    shll8   r4
    mov.l   @(job_position,r13),r0
    or      r0,r4
    mov     #2,r5

! The job's end: r4 the result into the result word, r0 r5, 1 done or 2 failed.
transform_end:
    mov.l   @(job_result_at,r13),r1
    mov.l   r4,@r1
    mov     r5,r0
    lds.l   @r15+,pr
    rts
    nop

vector_count:
    .word   252
    .balign 4
job_address:
    .long   module_end
imports_address:
    .long   module_imports

! The image's last longword, the CRC the package tool writes.
    .long   0
    .global module_end
module_end:
