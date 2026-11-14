/*
 * The SH-2 client of the CD block's SH-1 running the demo's card firmware. The client owns every
 * CD block command, and none is sent from an interrupt handler. Transfers go through the SH-2's DMA
 * controller in longwords, on SH1_DMAC_CHANNEL.
 *
 * The CD block, for a reader new to it. It is a small computer of its own inside the Saturn: an
 * SH-1 with Sega's firmware in ROM, which runs the drive and keeps what it reads in a buffer of 200
 * sectors. The SH-2 never touches the drive or that buffer. It only talks to the SH-1, through a
 * handful of registers:
 *
 * - CR1 to CR4, four 16-bit registers. The SH-2 writes a command into them, and the write of CR4
 *   hands it over. The SH-1 then writes its answer into the same four.
 * - HIRQ, the flags the SH-1 raises: CMOK once an answer is ready, DRDY once data can cross, EHST
 *   at a transfer's end, and so on. The SH-1 only sets them. The SH-2 clears a flag by writing a 0
 *   into its bit, so writing ~flag clears that flag and leaves the others.
 * - The data port, one address that sectors cross through, a word at a time.
 *
 * A command is its number in CR1's high byte and its parameters in the rest. The answer's first
 * byte, CR1's high byte, is the status: 0xff says the command was rejected, and bit 7 that the CD
 * block cannot take it just now.
 *
 * The buffer is shared out in 24 partitions. A sector never goes into a partition directly: it
 * goes through one of 24 filters, each connected to a partition. sh1_reset_selector() connects
 * filter n to partition n, so in this client "through filter n" and "into partition n" are the
 * same place.
 *
 * The MPEG card slot is where the demo comes in. Sega's firmware loads extra SH-1 code from a card
 * in that slot (E0), and 93 turns it on. The demo's card carries our own firmware, which adds
 * three commands: AD (who are you), AB (run this job) and AC (how is the job doing). A job is work
 * for the SH-1: an operation of the firmware's own, or a module's, a small SH-1 program the
 * firmware installs. A job's input and output are sectors in partitions. So the SH-2 puts the
 * input in, submits the job, waits for MPED, the flag the firmware raises at a job's end, and gets
 * the output back.
 */

#ifndef SH1_CLIENT_H
#define SH1_CLIENT_H

#include <stdbool.h>
#include <stdint.h>

/* The CD block's interrupt flags (HIRQ). */
#define SH1_CMOK 0x0001
#define SH1_DRDY 0x0002
#define SH1_ESEL 0x0040
#define SH1_EHST 0x0080
#define SH1_ECPY 0x0100
#define SH1_MPED 0x0800

#define SH1_STATUS_REJECT 0xFF
#define SH1_STATUS_WAIT 0x80

#define SH1_SECTOR_BYTES 2048
#define SH1_DMAC_CHANNEL 0

/* The card firmware's protocol versions, AD's CR1 low byte. They are no release numbers: they tell
 * the client which of the demo's firmwares answers, so one client and one set of discs run on
 * both. 2 is the assembly firmware, with the library check, 1 the card firmware in C, without. */
#define SH1_PROTOCOL 2
#define SH1_PROTOCOL_OLDEST 1
#define SH1_PROTOCOL_LIBRARY_CHECK 2

/* The firmware's operations, AB's CR1 low byte. A module's handle names its jobs: 3 to 250 for an
 * uploaded module, as its load gives them. */
#define SH1_OP_PING 0
#define SH1_OP_LOAD 1
#define SH1_OP_UNLOAD 2

/* The card ROM's library, its modules on fixed handles. A job sent to one loads it from the card
 * ROM first when another module is in the slot, so it needs no upload and no handle from a load. */
#define SH1_LIBRARY_FIRST 251
#define SH1_LIBRARY_DECOMPRESS 251
#define SH1_LIBRARY_TRANSFORM 252
#define SH1_LIBRARY_CIPHER 253

/* Bit 0 of a load's argument, with a library handle in the partition's place: check the CRC of
 * the card ROM's copy too. */
#define SH1_LOAD_CHECK 0x00000001UL

/* What sh1_library_check() finds for a handle. */
typedef enum {
        SH1_LIBRARY_OK,         /* in the card ROM, whole, and now the installed module */
        SH1_LIBRARY_MISSING,    /* the card ROM's library has no module on the handle */
        SH1_LIBRARY_BAD,        /* its copy fails the load, the reason in the result's high byte */
        SH1_LIBRARY_UNCHECKED   /* the firmware has no check (protocol 1), or gave no finding */
} sh1_library_t;

/* A job's states, AC's CR1 low byte. */
#define SH1_JOB_NONE 0
#define SH1_JOB_SUBMITTED 1
#define SH1_JOB_RUNNING 2
#define SH1_JOB_DONE 3
#define SH1_JOB_FAILED 4

/* The job record AC gives. */
typedef struct {
        uint8_t state;
        uint16_t id;
        uint32_t result;
} sh1_job_t;

/* The answer to the last command, CR1..CR4. */
extern uint16_t sh1_answer[4];

/* The protocol version the firmware gave at the last sh1_identity(), 0 before one succeeds. */
extern uint8_t sh1_protocol;

/* Sends a command and keeps its answer. With wait, also waits for one of those flags, and a command
 * rejected or told to wait fails. */
bool sh1_command(uint16_t cr1, uint16_t cr2, uint16_t cr3, uint16_t cr4, uint16_t wait);

/* 48 with flags fc: every partition emptied, every filter cleared and each filter's true output
 * back on the partition of the same number. */
bool sh1_reset_selector(void);

/* E0 subcode 1 loads the card's firmware. True when E1 shows the card checks passed and 01 a
 * nonzero MPEG version, which a failed load leaves at 0. The last answer is then 01's. */
bool sh1_card_load(void);

/* 93 with bit 0: the loaded card turned on. */
bool sh1_card_on(void);

/*
 * Transfers in two halves, so the SH-2 can work while its DMA controller moves the sectors through
 * the data port: a start, which returns once the DMA runs, and sh1_transfer_end(). One transfer is
 * open at a time, and no other command goes to the CD block until it ends.
 *
 * - sh1_put_start: 64 puts count sectors from data through filter. data is 4-byte aligned.
 * - sh1_get_start: 61 gets count sectors from position in partition into data, or 63 with take,
 *   which also deletes them once the transfer ends. data is 16-byte aligned, as the cache is
 *   purged over it.
 *
 * A start is false when a transfer is already open, or when the CD block refuses it, which is then
 * closed with 06.
 */
bool sh1_put_start(uint8_t filter, uint16_t count, const void *data);
bool sh1_get_start(uint8_t partition, uint16_t position, uint16_t count, void *data, bool take);

/* Whether a transfer is open, and whether its DMA still runs. */
bool sh1_transfer_open(void);
bool sh1_transfer_busy(void);

/* Waits for the open transfer's DMA, then 06 ends it. True when 06 counts every word. */
bool sh1_transfer_end(void);

/* A whole put or get: the start, then the end. */
bool sh1_put(uint8_t filter, uint16_t count, const void *data);
bool sh1_get(uint8_t partition, uint16_t position, uint16_t count, void *data);

/* 62 deletes count sectors from position in partition. */
bool sh1_delete(uint8_t partition, uint16_t position, uint16_t count);

/* 51: the sectors partition holds. */
bool sh1_sectors(uint8_t partition, uint16_t *count);

/* 30: the drive's output, CD device 0, connected to filter, or with sh1_disconnect() to none, so no
 * more sectors arrive. Each waits for ESEL, set once the change is made. */
bool sh1_connect(uint8_t filter);
void sh1_disconnect(void);

/* 10: the drive plays count sectors from frame address fad, which arrive through the connected
 * filter. Returns once the play is accepted, the sectors come after. */
bool sh1_play(uint32_t fad, uint16_t count);

/* 66 moves count sectors from position in partition through filter into its partition, inside the
 * CD block, and waits for ECPY, set at the move's end. Nothing crosses the data port. */
bool sh1_move(uint8_t partition, uint16_t position, uint16_t count, uint8_t filter);

/*
 * Reads count sectors of the disc, from frame address fad, into the CD block's buffer: the drive
 * plays them and the CD block files each one through filter, into the partition the filter is
 * connected to. Nothing crosses to the SH-2.
 *
 * A frame address (FAD) numbers the disc's sectors, 75 a second of play, with the first data
 * sector at 150. The filter must pass every sector, as sh1_reset_selector() leaves it.
 *
 * - 30 connects the drive's output (CD device 0) to filter.
 * - 10 plays from fad for count sectors.
 * - 51 is asked for the partition's sector count until it holds count more than before.
 *
 * True once they have all arrived, the drive left connected to filter. Waits some seconds at
 * most. A read that gives up disconnects the drive, so no sector arrives after it.
 */
bool sh1_read(uint8_t filter, uint32_t fad, uint16_t count);

/* AD: true when the firmware answers its identity and a protocol version from SH1_PROTOCOL_OLDEST
 * to SH1_PROTOCOL, with the activations since the card's load. The version is kept in
 * sh1_protocol. */
bool sh1_identity(uint16_t *activations);

/* AB submits a job: an operation or a module's handle, a job ID and the argument. False when it is
 * rejected: an unknown operation or handle, an argument the operation refuses, or a job already out. */
bool sh1_submit(uint8_t operation, uint16_t id, uint32_t argument);

/* AC: the job record. */
bool sh1_status(sh1_job_t *job);

/* Waits for job id to end: MPED, or some seconds without it, then AC as the record, since MPED also
 * ends other requests. True when the record shows the job done or failed. False when AC fails, the
 * record names no job or another, or the wait gives up with the job still out. */
bool sh1_wait(uint16_t id, sh1_job_t *job);

/* Loads a module as job id: its package, whole sectors at a 4-byte aligned address, put through
 * filter partition into the partition of the same number as sh1_reset_selector connects them, AB
 * operation 1, the wait, then the partition's sectors deleted.
 * - The partition holds nothing else, and sectors is the package's bytes rounded up to whole
 *   sectors. Otherwise the load fails.
 * - True when loaded and deleted, with the handle in job->result. job->state is the load's own
 *   outcome: SH1_JOB_DONE means the module is installed, and the handle before it rejected, also
 *   when the delete then failed. A failed load's result has the reason in its high byte.
 * - Every path deletes the partition's sectors but a failed wait, which leaves the package, since
 *   the load may still read it. */
bool sh1_module_load(uint8_t partition, const void *package, uint16_t sectors, uint16_t id,
    sh1_job_t *job);

/*
 * Checks one of the card ROM library's modules, as job id: AB operation 1 with the handle in the
 * partition's place and SH1_LOAD_CHECK, which loads the module from the card ROM and checks its
 * CRC on the way, then the wait. A game runs it at boot for each module it needs, so a card with an
 * older library says so before the first job fails.
 * - Call it after sh1_identity(), with no job out. A handle below SH1_LIBRARY_FIRST is not the
 *   library's, and is missing without a command sent.
 * - Only the firmware's reject means missing. No answer, or an answer to wait, is unchecked.
 * - The module checked last stays installed, since the SH-1 has one module slot. A job on any
 *   other library handle still loads its module first, as before.
 * - job holds the load's record once the firmware answered, and a bad copy's reason.
 */
sh1_library_t sh1_library_check(uint8_t handle, uint16_t id, sh1_job_t *job);

#endif /* SH1_CLIENT_H */
