! The card firmware's boundary with the CD block's own firmware: the parts C cannot say.
!
! - The entries. The firmware calls the header's entries in conventions of its own, inside the
!   command interrupt or in its tasks, on stacks with little room: task 9's has about 168 bytes,
!   80 of them for an interrupt's frame. Each entry here moves to a stack of this firmware's own,
!   keeps r1..r7 and PR there, calls the C and comes back.
! - The fillers, for hooks this firmware does not use.
! - The ROM's routines and the kernel's traps, wrapped for C callers.
! - The module call, which keeps r8..r13 for C, since a module may change them.
!
! C is compiled for the SH-1 with r14 left alone: the tasks resumed with only PC and SR take r14,
! r15 and GBR as they find them.

    .equ    work_request, 0x09075388        ! bit 0 makes task 9's card work loop call entry 54
    .equ    next_request, 0x09075389        ! bits 0..3 make the loop jump to entries 78, 77, 80 and 79
    .equ    sector_lock_task9, 0x0b090000   ! the sector lock, owner task 9
    .equ    rom_entry_34, 0xf9c0            ! the ROM table's entry 34
    .equ    stub_task8_wait, 0xf80a         ! entry 32, task 8's wait
    .equ    stub_recheck, 0xf8ba            ! entry 76, the card work loop's recheck
    .equ    stub_reject, 0xf892             ! entry 66, status ff with the MPEG status
    .equ    stub_slot_take, 0xf8ce          ! entry 81, a free slot under the sector lock
    .equ    stub_slot_clear, 0xf8d2         ! entry 82, clear 24 or 280 bytes
    .equ    stub_slot_address, 0xf8da       ! entry 84, a slot's address
    .equ    stub_slot_file, 0xf8e6          ! entry 87, file a slot through a filter
    .equ    stub_status_byte, 0xf8ee        ! service 89, the status byte in r12
    .equ    stub_slot_at, 0xf902            ! service 94, the slot at a position of a partition
    .equ    stub_slot_return, 0xf90e        ! service 97, return a slot to the pool

! Onto the stack whose end is at the literal stack: the old SP, PR and r1..r7 kept there. Uses r0.
    .macro  enter_c stack
    mov.l   \stack,r0
    mov.l   r15,@-r0
    mov     r0,r15
    sts.l   pr,@-r15
    mov.l   r1,@-r15
    mov.l   r2,@-r15
    mov.l   r3,@-r15
    mov.l   r4,@-r15
    mov.l   r5,@-r15
    mov.l   r6,@-r15
    mov.l   r7,@-r15
    .endm

! Back: r7..r1 and PR restored, then the old SP. Keeps r0, the C's result.
    .macro  leave_c
    mov.l   @r15+,r7
    mov.l   @r15+,r6
    mov.l   @r15+,r5
    mov.l   @r15+,r4
    mov.l   @r15+,r3
    mov.l   @r15+,r2
    mov.l   @r15+,r1
    lds.l   @r15+,pr
    mov.l   @r15,r15
    .endm

! Calls the function at the literal function.
    .macro  call function
    mov.l   \function,r0
    jsr     @r0
    nop
    .endm

! PR and r8..r13 onto the stack, and back.
    .macro  keep
    sts.l   pr,@-r15
    mov.l   r8,@-r15
    mov.l   r9,@-r15
    mov.l   r10,@-r15
    mov.l   r11,@-r15
    mov.l   r12,@-r15
    mov.l   r13,@-r15
    .endm

    .macro  restore
    mov.l   @r15+,r13
    mov.l   @r15+,r12
    mov.l   @r15+,r11
    mov.l   @r15+,r10
    mov.l   @r15+,r9
    mov.l   @r15+,r8
    lds.l   @r15+,pr
    .endm

    .text
    .balign 4

! Entry +4, called from 0x1b42 in task 9 with r1 the header once the image is decoded.
    .global _entry_load
_entry_load:
    enter_c job_stack_end
    call    card_load_address
    leave_c
    rts
    nop

! Entry 34, called by task 8's activation (0xa41c) after the MPEG state is cleared. Then the ROM
! table's entry 34, as the known cards do: it sets the MPEG state, the card bit that opens groups 9
! and A among it, and returns.
    .global _entry_activate
_entry_activate:
    enter_c task8_stack_end
    call    card_activate_address
    leave_c
    mov.l   rom_activate,r0
    mov.l   @r0,r0
    jmp     @r0
    nop

! Entries 27..29, commands AB, AC and AD, inside the command interrupt (IRQ6) with r1 CR1:CR2 and
! r2 CR3:CR4. The C returns whether it answers. The answer goes through service 89, which writes
! the status byte, takes r12 and r13 for CR1..CR4, posts a nonzero r11 and returns to the IRQ6
! handler.
    .global _entry_submit
_entry_submit:
    enter_c irq_stack_end
    mov     r1,r4
    mov     r2,r5
    call    command_submit_address
    leave_c
    bra     respond
    nop

    .global _entry_status
_entry_status:
    enter_c irq_stack_end
    call    command_status_address
    leave_c
    bra     respond
    nop

    .global _entry_identity
_entry_identity:
    enter_c irq_stack_end
    call    command_identity_address
    leave_c

respond:
    tst     r0,r0
    bt      _entry_reject
    mov.l   reply_address,r0
    mov.l   @r0,r11
    mov.l   @(4,r0),r12
    mov.l   @(8,r0),r13
    mov.l   status_byte_stub,r0
    jmp     @r0
    nop

! Every rejected command: entry 66 answers status ff, and nothing is posted.
    .global _entry_reject
_entry_reject:
    mov.l   reject_stub,r0
    jmp     @r0
    xor     r11,r11

! Entry 54, called by task 9's card work loop (0xba5e) while bit 0 of the work request is set.
    .global _entry_run
_entry_run:
    enter_c job_stack_end
    call    card_run_address
    leave_c
    rts
    nop

! The fillers. Called with a return address, return.
    .global _entry_ignore
_entry_ignore:
    rts
    nop

! Jumped to from task 8's message dispatch, go to its wait.
    .global _entry_task8_ignore
_entry_task8_ignore:
    mov.l   task8_wait_stub,r0
    jmp     @r0
    nop

! Called by task 9's card work loop for bit 1, 2 or 3 of the work request: clear all three, or the
! loop calls again. and.b is one instruction, so an AB inside IRQ6 cannot come between the read
! and the write and lose bit 0.
    .global _entry_request_ignore
_entry_request_ignore:
    stc     gbr,r2
    mov.l   work_request_address,r1
    ldc     r1,gbr
    mov     #0,r0
    and.b   #241,@(r0,gbr)
    ldc     r2,gbr
    rts
    nop

! Jumped to by the loop for bit 0, 1, 2 or 3 of the next request byte: clear all four, then the
! loop's recheck through entry 76. No interrupt handler sets bits in this byte.
    .global _entry_jump_ignore
_entry_jump_ignore:
    mov.l   next_request_address,r1
    mov.b   @r1,r0
    and     #240,r0
    mov.b   r0,@r1
    mov.l   recheck_stub,r0
    jmp     @r0
    nop

    .balign 4
job_stack_end:
    .long   job_stack + job_stack_bytes
task8_stack_end:
    .long   task8_stack + task8_stack_bytes
irq_stack_end:
    .long   irq_stack + irq_stack_bytes
card_load_address:
    .long   _card_load
card_activate_address:
    .long   _card_activate
command_submit_address:
    .long   _command_submit
command_status_address:
    .long   _command_status
command_identity_address:
    .long   _command_identity
card_run_address:
    .long   _card_run
reply_address:
    .long   _reply
rom_activate:
    .long   rom_entry_34
status_byte_stub:
    .long   stub_status_byte
reject_stub:
    .long   stub_reject
task8_wait_stub:
    .long   stub_task8_wait
recheck_stub:
    .long   stub_recheck
work_request_address:
    .long   work_request
next_request_address:
    .long   next_request

! The sector lock: trap 42 acquires it, 39 releases it, both with r11 the lock word.
    .global _sector_lock
_sector_lock:
    keep
    mov.l   sector_lock_word,r11
    trapa   #42
    restore
    rts
    nop

    .global _sector_unlock
_sector_unlock:
    keep
    mov.l   sector_lock_word,r11
    trapa   #39
    restore
    rts
    nop

! The ROM's routines, through their stubs. Each wrapper puts its arguments where the stub takes
! them and r0 the stub, then goes on in rom_call, which keeps PR and r8..r13 around the stub and
! returns to the C with the stub's r0, or rom_call_r1 with its r1.
rom_call:
    keep
    jsr     @r0
    nop
    restore
    rts
    nop

rom_call_r1:
    sts.l   pr,@-r15
    bsr     rom_call
    nop
    lds.l   @r15+,pr
    rts
    mov     r1,r0

! r4 a partition, r5 a position: the stub's r1, its low byte 0xff for none.
    .global _rom_slot_at
_rom_slot_at:
    mov     r4,r1
    mov     r5,r2
    mov.l   slot_at_stub,r0
    bra     rom_call_r1
    nop

! r4 a slot: its address, in r1.
    .global _rom_slot_address
_rom_slot_address:
    mov     r4,r1
    mov.l   slot_address_stub,r0
    bra     rom_call_r1
    nop

! A free slot in r0, or -1.
    .global _rom_slot_take
_rom_slot_take:
    mov.l   slot_take_stub,r0
    bra     rom_call
    nop

! r4 the address, in r1, and r5 0 for 24 bytes or 1 for 280.
    .global _rom_slot_clear
_rom_slot_clear:
    mov     r4,r1
    mov.l   slot_clear_stub,r0
    bra     rom_call
    nop

! The slot and the filter from RAM, where the C has put them.
    .global _rom_slot_file
_rom_slot_file:
    mov.l   slot_file_stub,r0
    bra     rom_call
    nop

! r4 a slot, in r1.
    .global _rom_slot_return
_rom_slot_return:
    mov     r4,r1
    mov.l   slot_return_stub,r0
    bra     rom_call
    nop

! r4 the pass, r5 the argument and r6 the result word, as the module takes them, r7 its entry.
    .global _module_call
_module_call:
    keep
    jsr     @r7
    nop
    restore
    rts
    nop

    .balign 4
sector_lock_word:
    .long   sector_lock_task9
slot_at_stub:
    .long   stub_slot_at
slot_address_stub:
    .long   stub_slot_address
slot_take_stub:
    .long   stub_slot_take
slot_clear_stub:
    .long   stub_slot_clear
slot_file_stub:
    .long   stub_slot_file
slot_return_stub:
    .long   stub_slot_return

! The stacks, after the image and before the module slot, never in the image. Task 9's holds about
! 170 bytes at its deepest (the C, these wrappers and a module's 32), then a ROM routine's own and
! an interrupt's 80-byte frame with its handler's start. An interrupt in task 8 or in IRQ6 also
! stacks its frame on theirs. They fill what the image leaves before the slot.
    .equ    job_stack_bytes, 448
    .equ    task8_stack_bytes, 176
    .equ    irq_stack_bytes, 176

    .section .stacks,"aw",@nobits
    .balign 4
job_stack:
    .space  job_stack_bytes
task8_stack:
    .space  task8_stack_bytes
irq_stack:
    .space  irq_stack_bytes
