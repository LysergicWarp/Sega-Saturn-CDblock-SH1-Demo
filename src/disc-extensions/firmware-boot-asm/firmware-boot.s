! Disc extension that installs the demo's card firmware with no MPEG card in the slot. The CD-block
! firmware decodes it to 0x09075800 after an accepted disc authentication, when the disc header's
! +0xf8 (FAD) and +0xfc (encoded length) name it.
!
! The entry copies the card firmware's image, carried after the code, to the expansion area at
! 0x0907b000, where the expansion loader would decode it from a card. It calls the image's load
! entry as that loader does, which fills the table's empty entries from the ROM table. Then it sets
! bit 1 of the hardware flag 0x0f00027d, MPEG card present, which startup sets only when its card
! test passes and which groups 9 and A need. 93 then activates the firmware as it would a card's,
! and AB, AC and AD reach it.
!
! - Each accepted disc clears the expansion area before the disc extension is read, so every
!   authentication of this disc installs a fresh image: no module, the job and the activations 0.
! - The MPEG version 0x0f0002fd stays 0, as no expansion load ran, so E2 stays rejected.
! - With bit 1 set, E0 subcode 1 runs the card checks. With no card they fail and clear the
!   expansion area, and the next authentication of this disc installs the image again.
! - The hardware flag is left alone by task 6's setup, so bit 1 stays set until a CD-block reset,
!   whose startup works it out again and clears the disc extension.
! - VBR stays 0, the ROM's vector table, and no resume word is set: the firmware runs only where the
!   ROM calls into the expansion area, as a card's would.

    .equ    expansion, 0x0907b000
    .equ    hardware_flag, 0x0f00027d
    .equ    card_present, 2                 ! bit 1 of the hardware flag

    .text
    .global boot_start
boot_start:
! Loader header, 0x48 bytes.
    .long   0                       ! +0x00 VBR, the ROM's vector table
    .fill   14,4,0                  ! +0x04..+0x38 task 1..14 resume addresses, none
    .long   boot_entry              ! +0x3c entry
    .long   0                       ! +0x40 callback for a card extension load, none
    .byte   0xb0                    ! +0x44 CD block revision, nonzero to install, 01 reports it
    .byte   0                       ! +0x45 compared with a card extension's byte +1
    .word   0

! Entry, called from 0x1ac6 with SR 0xf0 on task 7's stack, PR 0x1aca. Every register but r14 and
! r15 is free, PR and the stack must come back as they were.
    .balign 4
    .global boot_entry
boot_entry:
    sts.l   pr,@-r15
    mov.l   entry_image,r1
    mov.l   entry_expansion,r2
    mov.l   entry_image_end,r3

boot_copy:
    mov.l   @r1+,r0
    mov.l   r0,@r2
    add     #4,r2
    cmp/hs  r3,r1
    bf      boot_copy
! The image's load entry at +4, called as the expansion loader calls it: r0 its address, r1 the
! header. It uses r0 and r2..r4 and returns.
    mov.l   entry_expansion,r1
    mov.l   @(4,r1),r0
    jsr     @r0
    nop
    mov.l   entry_flag,r1
    mov.b   @r1,r0
    or      #card_present,r0
    mov.b   r0,@r1
    lds.l   @r15+,pr
    rts
    nop

    .balign 4
entry_image:
    .long   boot_image
entry_image_end:
    .long   boot_image_end
entry_expansion:
    .long   expansion
entry_flag:
    .long   hardware_flag

! The card firmware's image as its own build links it, at 0x0907b000, a whole number of longwords.
    .balign 4
    .global boot_image
boot_image:
    .incbin "card-firmware.bin"
    .global boot_image_end
boot_image_end:
    .global boot_end
boot_end:
