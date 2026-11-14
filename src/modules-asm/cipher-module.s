! A module for the card firmware, ../card-firmwares/card-firmware-asm/card-firmware.s, that
! decrypts sectors with XTEA in counter mode, one input sector into one output sector per pass. Its
! package is its image, linked into the module slot by cipher-module.ld, with the CRC that
! ../../tools/package.py writes. ../../tools/cipher.py encrypts for it.
!
! The argument: CR3's high byte the input partition and its low byte the output filter, both up to
! 23, and CR4 the stream number, which picks the key stream. The result is the sectors filed.
!
! - Counter mode: each 8 bytes are XORed with XTEA's encryption of a counter block, the stream
!   number and the block's number from the job's first sector, 256 a sector, both as longwords.
!   Encrypting and decrypting are the same operation. The key, the 16 bytes "SH-1 demo cipher", is
!   built into the module.
! - Each input sector, 2048 bytes from where its mode puts its data. Each output sector holds the
!   2048 bytes XORed, and reads as a mode 0 sector, so 61 with get sector length 0 sends them.
! - A failed job's result is the reason in the high byte and the sectors filed below it. Reasons: 1
!   no input sector, 5 no free slot, 6 a mode 2 form 2 input sector, 7 an argument out of range.
! - The input stays in its partition, and the sectors filed stay in the filter's, whether the job is
!   done or failed. The host leaves both partitions alone until MPED, then deletes them.
!
! XTEA (Needham and Wheeler, 1997): 32 cycles on a 64-bit block (v0, v1), each
!   v0 += (((v1 << 4) ^ (v1 >> 5)) + v1) ^ (sum + key[sum & 3])
!   sum += 0x9e3779b9
!   v1 += (((v0 << 4) ^ (v0 >> 5)) + v0) ^ (sum + key[(sum >> 11) & 3])
! from sum 0, all in 32 bits.
!
! Reading this file. Its frame is transform-module.s's: the same header, entry, services and job
! record, and that file's notes on modules and on SH-1 assembly apply here. Only the work on each
! sector differs.
!
! Why counter mode. XTEA turns one 8-byte block into another. In counter mode it never touches the
! data itself: it encrypts a counter, a number that is different for every block, and the result is
! XORed onto the data. Three things follow, each of use here:
! - XOR undoes itself, so the module needs XTEA's encryption only, never its decryption.
! - Each block's counter is known from where the block is (the stream number, and the block's
!   number), so any sector decrypts by itself, in any order. The scroller relies on that: each
!   strip is a job of its own.
! - Two files must never share a stream number under one key, or their key streams would be equal.
!   The scroller gives every strip its own.
!
! Why XTEA. It needs only adds, XORs and shifts on 32-bit registers, with no tables, which suits a
! CPU with no cache and a 16 KB module slot. The SH-1 has no instruction that shifts by a chosen
! count, so the shifts by 4, 5 and 11 below are built from its shifts by 1, 2 and 8. At 32 cycles a
! block and 256 blocks a sector, a pass here is the longest of the three modules'.

    .equ    import_count, 4
    .equ    stack_depth, 4                  ! PR at the entry
    .equ    blocks, 256                     ! the 8-byte blocks of a sector

! The services, by offset in the import table once the load has bound it.
    .equ    slot_at, 0
    .equ    sector_data, 4
    .equ    take, 8
    .equ    file, 12

! The job, in the BSS after the image, by offset from its start.
    .equ    job_result_at, 0                ! the result word's address, from the entry's r6
    .equ    job_partition, 4                ! the input partition
    .equ    job_filter, 8                   ! the output filter
    .equ    job_stream, 12                  ! the stream number
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
! decrypts the input sector at position n, and the pass that finds none ends the job.
module_entry:
    sts.l   pr,@-r15
    mov.l   job_address,r13
    mov.l   r6,@(job_result_at,r13)
    mov.l   r4,@(job_position,r13)
    tst     r4,r4
    bf      cipher_sector
! The job's start: the argument checked and kept.
    mov     r5,r0
    shlr16  r0
    mov     r0,r1
    shlr8   r1
    extu.b  r0,r0
    mov     #24,r2
    cmp/hs  r2,r1
    bt      cipher_argument_refused
    cmp/hs  r2,r0
    bt      cipher_argument_refused
    mov.l   r0,@(job_filter,r13)
    mov.l   r1,@(job_partition,r13)
    extu.w  r5,r0
    mov.l   r0,@(job_stream,r13)

! The input sector at this pass's position, with r8 its data. None left ends the job, with none at
! all reason 1.
cipher_sector:
    mov.l   @(job_partition,r13),r4
    mov.l   @(job_position,r13),r5
    mov.l   imports_address,r1
    mov.l   @(slot_at,r1),r0
    jsr     @r0
    nop
    cmp/pz  r0
    bt      cipher_input
    mov.l   @(job_position,r13),r4
    tst     r4,r4
    bt      cipher_no_input
    bra     cipher_end
    mov     #1,r5

cipher_input:
    mov     r0,r4
    mov.l   imports_address,r1
    mov.l   @(sector_data,r1),r0
    jsr     @r0
    nop
    tst     r0,r0
    bt      cipher_form2
    mov     r0,r8
! A free slot for the output, with r9 its data. No free slot: reason 5.
    mov.l   imports_address,r1
    mov.l   @(take,r1),r0
    jsr     @r0
    nop
    cmp/eq  #-1,r0
    bt      cipher_no_slot
    mov.l   r0,@(job_slot,r13)
    mov     r0,r4
    mov.l   imports_address,r1
    mov.l   @(sector_data,r1),r0
    jsr     @r0
    nop
    mov     r0,r9

! The sector, from r8 to r9, a block at a time: r10 the block's number, r11 the blocks left, r12
! the key, r6 XTEA's constant.
    mov.l   @(job_position,r13),r10
    shll8   r10
    mov.w   block_count,r11
    mov.l   key_address,r12
    mov.l   delta,r6

cipher_block:
! The counter block (the stream number, the block's number) through XTEA's 32 cycles: r1 v0, r2 v1,
! r3 sum, r7 the cycles left, r4 and r5 the terms.
    mov.l   @(job_stream,r13),r1
    mov     r10,r2
    mov     #0,r3
    mov     #32,r7

cipher_cycle:
! v0 += (((v1 << 4) ^ (v1 >> 5)) + v1) ^ (sum + key[sum & 3])
    mov     r2,r4
    shll2   r4
    shll2   r4
    mov     r2,r5
    shlr2   r5
    shlr2   r5
    shlr    r5
    xor     r5,r4
    add     r2,r4
    mov     r3,r0
    and     #3,r0
    shll2   r0
    mov.l   @(r0,r12),r5
    add     r3,r5
    xor     r5,r4
    add     r4,r1
    add     r6,r3
! v1 += (((v0 << 4) ^ (v0 >> 5)) + v0) ^ (sum + key[(sum >> 11) & 3])
    mov     r1,r4
    shll2   r4
    shll2   r4
    mov     r1,r5
    shlr2   r5
    shlr2   r5
    shlr    r5
    xor     r5,r4
    add     r1,r4
    mov     r3,r0
    shlr8   r0
    shlr2   r0
    shlr    r0
    and     #3,r0
    shll2   r0
    mov.l   @(r0,r12),r5
    add     r3,r5
    xor     r5,r4
    add     r4,r2
    add     #-1,r7
    tst     r7,r7
    bf      cipher_cycle

! The key stream's 8 bytes XORed into the block's.
    mov.l   @r8+,r0
    xor     r1,r0
    mov.l   r0,@r9
    mov.l   @r8+,r0
    xor     r2,r0
    mov.l   r0,@(4,r9)
    add     #8,r9
    add     #1,r10
    add     #-1,r11
    tst     r11,r11
    bf      cipher_block

! Filed through the job's filter. A job that is no longer its own has had the slot returned, and
! ends with nothing published.
    mov.l   @(job_slot,r13),r4
    mov.l   @(job_filter,r13),r5
    mov.l   imports_address,r1
    mov.l   @(file,r1),r0
    jsr     @r0
    nop
    tst     r0,r0
    bt      cipher_dropped
    mov     #0,r0
    lds.l   @r15+,pr
    rts
    nop

cipher_argument_refused:
    mov     #0,r0
    mov.l   r0,@(job_position,r13)
    bra     cipher_fail
    mov     #7,r4

cipher_no_input:
    bra     cipher_fail
    mov     #1,r4

cipher_form2:
    bra     cipher_fail
    mov     #6,r4

cipher_no_slot:
    bra     cipher_fail
    mov     #5,r4

cipher_dropped:
    bra     cipher_fail
    mov     #0,r4

! A failed job, r4 the reason: the result is the reason in its high byte and the sectors filed below.
cipher_fail:
    shll16  r4
    shll8   r4
    mov.l   @(job_position,r13),r0
    or      r0,r4
    mov     #2,r5

! The job's end: r4 the result into the result word, r0 r5, 1 done or 2 failed.
cipher_end:
    mov.l   @(job_result_at,r13),r1
    mov.l   r4,@r1
    mov     r5,r0
    lds.l   @r15+,pr
    rts
    nop

block_count:
    .word   blocks
    .balign 4
job_address:
    .long   module_end
imports_address:
    .long   module_imports
key_address:
    .long   key
delta:
    .long   0x9e3779b9
! "SH-1 demo cipher", as four longwords.
key:
    .ascii  "SH-1 demo cipher"

! The image's last longword, the CRC the package tool writes.
    .long   0
    .global module_end
module_end:
