/*
 * What boundary.s gives the C side, and what it calls there.
 *
 * The firmware's ROM was not written for C callers: its routines take their arguments in r1, r2
 * and r5, and the entries it calls give theirs in r1 and r2 or expect answers in r11..r13. Each
 * function here is a small assembly wrapper that turns the C convention (arguments from r4, the
 * result in r0, r8..r14 kept) into the ROM's and back.
 */

#ifndef BOUNDARY_H
#define BOUNDARY_H

#include <stdbool.h>
#include <stdint.h>

/* The header's entries. Each switches to a stack of this firmware's own and calls the C below. */
void entry_load(void);
void entry_activate(void);
void entry_submit(void);
void entry_status(void);
void entry_identity(void);
void entry_run(void);

/* The rejection of a command (status ff), and the fillers for hooks this firmware does not use */
void entry_reject(void);
void entry_ignore(void);
void entry_task8_ignore(void);
void entry_request_ignore(void);
void entry_jump_ignore(void);

/* The C the entries call, in card-firmware.c */
void card_load(void);
void card_activate(void);
bool command_submit(uint32_t cr1_cr2, uint32_t cr3_cr4);
bool command_status(void);
bool command_identity(void);
void card_run(void);

/* A command's answer: r11 the message to post, 0 for none, then CR1:CR2 and CR3:CR4 */
struct reply {
        uint32_t post;
        uint32_t cr1_cr2;
        uint32_t cr3_cr4;
};

extern struct reply reply;

/*
 * The sector lock, which guards the CD buffer's slots, through the kernel's traps 42 and 39. A
 * contended acquire suspends task 9, and the kernel resumes it with only PC and SR restored, so
 * the wrappers keep r8..r13 and PR on the stack across the trap.
 */
void sector_lock(void);
void sector_unlock(void);

/* The ROM's slot routines */
#define CLEAR_HEADER 0          /* rom_slot_clear(): 24 bytes */
#define CLEAR_TAIL 1            /* 280 bytes */

uint32_t rom_slot_at(uint32_t partition, uint32_t position);   /* low byte 0xff for none */
uint8_t *rom_slot_address(uint32_t slot);
int32_t rom_slot_take(void);                                    /* -1 for none */
void rom_slot_clear(uint8_t *at, uint32_t form);
void rom_slot_file(void);
void rom_slot_return(uint32_t slot);

/* A pass of the installed module's job, at entry. The module may change r8..r13, kept here. */
int32_t module_call(uint32_t pass, uint32_t argument, uint32_t *result, uint32_t entry);

#endif /* BOUNDARY_H */
