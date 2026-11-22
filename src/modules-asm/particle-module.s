! A module for the card firmware, ../card-firmwares/card-firmware-asm/card-firmware.s, that moves
! particles of its own, kept in its BSS, under gravity with a bounce off a floor. Nothing crosses the
! data port: AB starts a job and AC gives its result. Its package is its image, linked into the
! module slot by particle-module.ld, with the CRC that ../../tools/package.py writes.
!
! The argument: its bit 31 (CR3's bit 15) resets the particles first, and its low 16 bits (CR4) are
! the steps each particle moves, 1 to 64. The result is the particles' checksum after the job.
!
! - 1280 particles of six int16, in this order: x, y, z and their velocities vx, vy, vz. Each step:
!   vy less 3, each velocity added to its position, and with y below 0, y and vy negated and vy less
!   a quarter of itself (shifted right by 2). x and z wrap at 16 bits.
! - A reset gives particle i: y (i & 63) << 6, vx ((i & 31) - 16) << 2, vz (((i >> 5) & 31) - 16)
!   << 2, the rest 0.
! - The checksum: s = 0, then for each particle's three longwords in order, x:y, z:vx and vy:vz,
!   s = (s rotated left by 1) + the longword.
! - A failed job's result is the reason in the high byte: 7 an argument out of range.
! - The job's first pass checks the argument and does the reset. Each later pass moves 64
!   particles, each through all its steps in registers and written back once, so a job touches the
!   CD buffer's RAM for the particles once per particle, not once per step.
!
! The frame is transform-module.s's: the same header, entry and job record, and that file's notes on
! modules and on SH-1 assembly apply here. This module calls no service, so it has no imports and
! uses no stack.

    .equ    import_count, 0
    .equ    stack_depth, 0
    .equ    particle_count, 1280
    .equ    particle_bytes, 12
    .equ    slice, 64                       ! the particles a pass moves
    .equ    steps_most, 64
    .equ    gravity, 3

    .if     particle_count % slice
    .error  "particle_count must be a whole number of slices"
    .endif

! The job, in the BSS after the image, by offset from its start, then the particles.
    .equ    job_result_at, 0                ! the result word's address, from the entry's r6
    .equ    job_steps, 4                    ! the steps each particle moves
    .equ    job_next, 8                     ! the next particle's address
    .equ    job_sum, 12                     ! the checksum so far
    .equ    job_particles, 16
    .equ    job_size, job_particles + particle_count * particle_bytes

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

! The entry, once per pass: r4 the pass, r5 the argument, r6 the result word's address. Pass 0 sets
! the job up, every later pass moves the next slice.
module_entry:
    mov.l   job_address,r13
    tst     r4,r4
    bf      particle_pass
! The job's start: the argument checked and kept, the checksum cleared and the first particle next.
    mov.l   r6,@(job_result_at,r13)
    extu.w  r5,r0
    tst     r0,r0
    bt      particle_refused
    mov     #steps_most,r1
    cmp/hi  r1,r0
    bt      particle_refused
    mov.l   r0,@(job_steps,r13)
    mov     #0,r0
    mov.l   r0,@(job_sum,r13)
    mov     r13,r8
    add     #job_particles,r8
    mov.l   r8,@(job_next,r13)
! Bit 31 clear: the particles stay as the last job left them.
    cmp/pz  r5
    bt      particle_set_up
! The reset, r7 the particle's number and r8 its address. mov.w with a displacement stores only r0,
! so each field is worked out in r0.
    mov     #0,r7
    mov.w   count_word,r9

particle_reset:
    mov     #0,r0
    mov.w   r0,@(0,r8)                      ! x
    mov.w   r0,@(4,r8)                      ! z
    mov.w   r0,@(8,r8)                      ! vy
    mov     r7,r0
    and     #63,r0
    shll2   r0
    shll2   r0
    shll2   r0
    mov.w   r0,@(2,r8)                      ! y, (i & 63) << 6
    mov     r7,r0
    and     #31,r0
    add     #-16,r0
    shll2   r0
    mov.w   r0,@(6,r8)                      ! vx, ((i & 31) - 16) << 2
    mov     r7,r0
    shlr2   r0
    shlr2   r0
    shlr    r0
    and     #31,r0
    add     #-16,r0
    shll2   r0
    mov.w   r0,@(10,r8)                     ! vz, (((i >> 5) & 31) - 16) << 2
    add     #particle_bytes,r8
    add     #1,r7
    cmp/eq  r9,r7
    bf      particle_reset

particle_set_up:
    rts
    mov     #0,r0

! A pass: r8 the particle, r9 the slice's particles left, r10 the steps, r11 the checksum. Each
! particle is read into r1..r6, sign-extended, and moved its steps there, r7 counting them. x and z
! may run past 16 bits in a register, and only their low 16 bits are stored, which is the 16-bit
! wrap. y and vy stay well inside 16 bits.
particle_pass:
    mov.l   @(job_next,r13),r8
    mov.l   @(job_steps,r13),r10
    mov.l   @(job_sum,r13),r11
    mov     #slice,r9

particle_next:
    mov.w   @r8+,r1                         ! x
    mov.w   @r8+,r2                         ! y
    mov.w   @r8+,r3                         ! z
    mov.w   @r8+,r4                         ! vx
    mov.w   @r8+,r5                         ! vy
    mov.w   @r8+,r6                         ! vz
    mov     r10,r7

particle_step:
    add     #-gravity,r5
    add     r4,r1
    add     r5,r2
    add     r6,r3
    cmp/pz  r2
    bt      particle_moved
! Below the floor: y and vy negated, then vy less vy >> 2, three quarters of the speed kept.
    neg     r2,r2
    neg     r5,r5
    mov     r5,r0
    shar    r0
    shar    r0
    sub     r0,r5

particle_moved:
    add     #-1,r7
    tst     r7,r7
    bf      particle_step
! Written back from the last field down, which leaves r8 at the particle again, then its three
! longwords into the checksum, which leaves r8 at the next.
    mov.w   r6,@-r8
    mov.w   r5,@-r8
    mov.w   r4,@-r8
    mov.w   r3,@-r8
    mov.w   r2,@-r8
    mov.w   r1,@-r8
    mov.l   @r8+,r0
    rotl    r11
    add     r0,r11
    mov.l   @r8+,r0
    rotl    r11
    add     r0,r11
    mov.l   @r8+,r0
    rotl    r11
    add     r0,r11
    add     #-1,r9
    tst     r9,r9
    bf      particle_next
! The slice kept for the next pass. Past the last particle the job ends, its result the checksum.
    mov.l   r8,@(job_next,r13)
    mov.l   r11,@(job_sum,r13)
    mov.l   particles_end,r0
    cmp/hs  r0,r8
    bt      particle_done
    rts
    mov     #0,r0

particle_done:
    mov.l   @(job_result_at,r13),r1
    mov.l   r11,@r1
    rts
    mov     #1,r0

! An argument out of range: reason 7 in the result's high byte.
particle_refused:
    mov     #7,r0
    shll16  r0
    shll8   r0
    mov.l   @(job_result_at,r13),r1
    mov.l   r0,@r1
    rts
    mov     #2,r0

count_word:
    .word   particle_count
    .balign 4
job_address:
    .long   module_end
particles_end:
    .long   module_end + job_size

! The image's last longword, the CRC the package tool writes.
    .long   0
    .global module_end
module_end:
