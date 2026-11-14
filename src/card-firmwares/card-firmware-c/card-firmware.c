/*
 * The card firmware, in C: the same firmware as ../card-firmware-asm/card-firmware.s, the same
 * commands, packages, services and limits, written to be read. That file's header comment is the
 * contract, and this one explains how the parts work.
 *
 * One thing the assembly firmware has and this one does not: the library check of protocol 2, a
 * load that names a library handle and can ask for the CRC. This firmware answers protocol 1 and
 * rejects such a load, as it has no room left for it: the image and its stacks end 16 bytes before
 * the module slot. The client works with both and reports the library as unchecked here.
 *
 * Where it runs. The CD block's SH-1 runs Sega's firmware, a small kernel with tasks that read the
 * disc, answer the Saturn's commands and fill the CD buffer. The firmware has a slot for the MPEG
 * card: when a card is present, an expansion loader decodes the card's code into RAM at
 * 0x0907b000 and calls it. From then on the firmware calls the card through a table of entries in
 * the card's header, at points of its own: a command from the Saturn, an activation, task 9's work
 * loop. That table is the only way in, and this firmware fills it with its own functions.
 *
 * What it does. The Saturn uploads modules, small programs of SH-1 code, into the module slot, then
 * runs jobs of them. A job runs in passes: task 9 calls entry 54 over and over while the job's
 * request bit is set, and each call runs one step. Between steps the firmware goes on reading the
 * disc and answering commands, so a long job never stops the drive.
 *
 * The library. The card ROM can also hold modules of its own, each on a fixed handle, 251 to
 * 255. A job sent to one of those needs no upload: when its module is not the one in the slot, the
 * job's first passes load it from the card ROM, as a load from a partition would but without the
 * CRC, and then run it. The Saturn's code names them by constants, as it would a library's
 * functions.
 *
 * The parts:
 * - The header, the table the firmware calls through.
 * - The commands AB, AC and AD, answered inside the CD block's command interrupt.
 * - The job's passes, run by task 9: ping, load, unload and the module's own jobs.
 * - The services modules call to use the CD buffer.
 * - The loader, which checks a package and installs it, from a partition or the card ROM.
 *
 * boundary.s holds what C cannot say: the entries' register conventions, the switch to this
 * firmware's own stacks, the calls into the firmware's ROM and its kernel traps.
 */

#include <stdbool.h>
#include <stdint.h>

#include "boundary.h"

/* AD's answer: the protocol version, and "DOSE". The version is no release number: it tells the
 * client which of the demo's firmwares answers. 1 is this one, 2 the assembly one. */
#define PROTOCOL_VERSION 1
#define IDENTITY 0x444f5345

/*
 * The firmware's own RAM this firmware uses, at addresses fixed in every revision.
 *
 * - work_request: bit 0 makes task 9's card work loop call entry 54, once per pass.
 * - filed_filter, filed_slot: the filter and the slot entry 87 files, read from RAM.
 * - interrupt_flags: the card's interrupt flags the Saturn reads, MPED among them.
 */
#define WORK_REQUEST (*(volatile uint8_t *)0x09075388)
#define FILED_FILTER (*(volatile uint8_t *)0x09075399)
#define FILED_SLOT (*(volatile uint16_t *)0x0907539a)
#define INTERRUPT_FLAGS (*(volatile uint16_t *)0x0a00001e)

#define REQUEST_JOB 0x01
#define FLAG_MPED 0x0800

/* The ROM's table of default entries, which fills those this firmware leaves empty */
#define ROM_ENTRIES ((void (*const *)(void))0x0000f938)

/* The message an accepted AB posts to task 9: task 9, class 0x81, payload 1 */
#define SUBMIT_MESSAGE 0x09810001

/* AB's operations below the handles. Each load hands out the next handle, 3 to 250, and 251 to
 * 255 are the library's. */
#define OPERATION_PING 0
#define OPERATION_LOAD 1
#define OPERATION_UNLOAD 2
#define OPERATION_COUNT 3
#define HANDLE_FIRST 3
#define HANDLE_LAST 250
#define LIBRARY_FIRST 251

/* The states AC reports */
#define STATE_NONE 0
#define STATE_SUBMITTED 1
#define STATE_RUNNING 2
#define STATE_DONE 3
#define STATE_FAILED 4

/* A sector's data in the CD buffer, and the most a package may take */
#define SECTOR_BYTES 2048
#define PACKAGE_MOST 8

/* The module slot, 16 KB at the end of the expansion area */
#define SLOT_START 0x0907c000
#define SLOT_BYTES 0x4000

/* A package's header: "DOSM", a module's stack at most 32 bytes */
#define MODULE_MAGIC 0x444f534d
#define STACK_MOST 32

/*
 * The card ROM, which the SH-1 reads directly from 0x0e000000. The expansion loader decodes only
 * its start, this firmware. The library's table is at 0x8000 in it: "DOSL", the count, then each
 * module's package by its offset in the ROM, the first on handle 251. tools/library.py writes it.
 */
#define CARD_ROM 0x0e000000

struct library {
        uint32_t magic;
        uint32_t count;
        uint32_t offsets[5];
};

#define LIBRARY ((const struct library *)(CARD_ROM + 0x8000))
#define LIBRARY_MAGIC 0x444f534c

/* Why a load failed, in its result's high byte */
#define LOAD_NO_SECTORS 1
#define LOAD_FORM2 2
#define LOAD_NOT_PACKAGE 3
#define LOAD_BOUNDS 4
#define LOAD_UNKNOWN_IMPORT 5
#define LOAD_CRC 6

/*
 * The record of the current job. AB writes it inside the command interrupt, task 9 runs the job
 * from it, AC reads it. Its layout is fixed: tools that read the image find it through the
 * header, as they find the assembly version's.
 */
struct record {
        uint32_t argument;              /* +0, CR3:CR4 of AB */
        uint16_t job_id;                /* +4, CR2 of AB */
        uint8_t operation;              /* +6 */
        uint8_t state;                  /* +7 */
        uint32_t result;                /* +8 */
        uint16_t activations;           /* +12, since the load */
        uint16_t job_activations;       /* +14, when the job started */
};

static volatile struct record record;

/* The installed module: its handle, 0 for none, the next load's handle, and its entry */
struct installed {
        uint8_t handle;
        uint8_t next_handle;
        uint16_t unused;
        uint32_t entry;
};

static volatile struct installed installed = {
        .next_handle = HANDLE_FIRST
};

/*
 * The job's own data, which only task 9 touches. A load keeps here what its first pass checked,
 * and the later passes work from that, not from the package again.
 */
static struct {
        uint32_t pass;                  /* the passes so far */
        uint32_t result;                /* the module's result word */
        uint32_t partition;             /* the package's partition */
        uint32_t count;                 /* the package's sectors */
        uint32_t done;                  /* the bytes the CRC has taken */
        uint32_t crc;                   /* the CRC so far */
        uint32_t image;                 /* the header's fields, checked */
        uint32_t bss;
        uint32_t table;
        uint32_t imports;
        uint32_t entry;
        const uint8_t *data[PACKAGE_MOST];      /* each sector's data */
} job;

/* What boundary.s answers a command with: r11 a message to post or 0, r12 CR1:CR2, r13 CR3:CR4 */
struct reply reply;

/*
 * Interrupts. The command interrupt writes the record, so task 9 reads and acts on it with
 * interrupts masked: SR's interrupt mask set to 15 holds off every maskable interrupt until SR is
 * restored.
 */
static inline uint32_t
interrupts_mask(void)
{
        uint32_t sr;

        __asm__ volatile ("stc sr,%0" : "=r" (sr));
        __asm__ volatile ("ldc %0,sr" : : "r" (sr | 0xf0) : "memory", "t");

        return sr;
}

static inline void
interrupts_restore(uint32_t sr)
{
        __asm__ volatile ("ldc %0,sr" : : "r" (sr) : "memory", "t");
}

extern uint8_t image_code[];
extern uint8_t image_end[];

/*
 * The expansion header, 0x16c bytes at the image's start. The firmware reads +0 to know a card is
 * there, calls +4 once the image is decoded, and calls the rest by number from then on.
 *
 * Entries 0..33 are MPEG commands 90..B1 (entry n is command 0x90 + n), the others are hooks in
 * the firmware's tasks. Entries left 0 here are filled from the ROM's table by card_load(), so the
 * ROM keeps handling 90, 91 and 92. 3, 30 and 31 stay 0 as on the known cards: 93, AE and AF have
 * handlers of their own. Every other hook the known cards fill gets a filler that does nothing
 * harmful, so that no call lands at address 0.
 */
struct expansion_header {
        uint8_t mpeg_version;           /* +0, nonzero for 93 */
        uint8_t disc_version;           /* +1, compared with a disc extension's */
        uint16_t unused;
        void (*entry)(void);            /* +4, called once decoded */
        void (*entries[89])(void);      /* +8, entries 0..88 */
        const void *tools[4];           /* +0x16c, for tools that read the image */
};

__attribute__ ((section(".header")))
struct expansion_header header = {
        .mpeg_version = 1,
        .disc_version = 1,
        .entry = entry_load,
        .entries = {
                [4 ... 26] = entry_reject,              /* commands 94..AA */
                [27] = entry_submit,                    /* AB */
                [28] = entry_status,                    /* AC */
                [29] = entry_identity,                  /* AD */
                [34] = entry_activate,                  /* task 8's activation */
                [43 ... 50] = entry_ignore,
                [51] = entry_task8_ignore,
                [52] = entry_ignore,
                [53] = entry_request_ignore,
                [54] = entry_run,                       /* task 9's card work loop */
                [55] = entry_ignore,
                [62] = entry_ignore,
                [63] = entry_ignore,
                [64] = entry_task8_ignore,
                [65] = entry_ignore,
                [69] = entry_request_ignore,
                [70] = entry_request_ignore,
                [71] = entry_ignore,
                [72] = entry_task8_ignore,
                [73 ... 75] = entry_ignore,
                [77 ... 80] = entry_jump_ignore,
                [85] = entry_ignore
        },
        .tools = { image_code, image_end, (const void *)&record, (const void *)&installed }
};

/*
 * Entry +4, once the image is decoded: every entry left 0 takes the ROM's, as the known cards'
 * do, so the ROM's routines and services stay reachable through the firmware's stubs.
 */
void
card_load(void)
{
        for (unsigned i = 0; i < 89; i++) {
                if (header.entries[i] == 0) {
                        header.entries[i] = ROM_ENTRIES[i];
                }
        }
}

/* Entry 34, from task 8's activation (93): a new activation, with no job */
void
card_activate(void)
{
        record.argument = 0;
        record.job_id = 0;
        record.operation = 0;
        record.state = STATE_NONE;
        record.result = 0;
        record.activations++;
}

/* Whether the card ROM's library holds a module on handle */
static bool
library_has(uint8_t handle)
{
        return handle >= LIBRARY_FIRST && LIBRARY->magic == LIBRARY_MAGIC &&
            (uint32_t)(handle - LIBRARY_FIRST) < LIBRARY->count;
}

/* AB and AC answer with the job's state and ID, and its result */
static void
reply_with_record(uint32_t post)
{
        reply.post = post;
        reply.cr1_cr2 = ((uint32_t)record.state << 16) | record.job_id;
        reply.cr3_cr4 = record.result;
}

/*
 * AB, submit: CR1's low byte the operation, CR2 the job ID, CR3:CR4 the argument. False rejects
 * it. Accepted, the job waits in the record, bit 0 of the work request asks task 9's loop to run
 * it and the reply's message wakes task 9.
 *
 * Task 9 changes the work request only with interrupts masked, and this runs inside an
 * interrupt, so nothing comes between its read and its write here.
 */
bool
command_submit(uint32_t cr1_cr2, uint32_t cr3_cr4)
{
        if (record.state == STATE_SUBMITTED || record.state == STATE_RUNNING) {
                return false;
        }

        const uint8_t operation = cr1_cr2 >> 16;

        if (operation >= OPERATION_COUNT) {
                /* A handle: the installed module's, or one of the library's, loaded by the job */
                if (operation != installed.handle && !library_has(operation)) {
                        return false;
                }
        } else if (operation == OPERATION_LOAD) {
                /* CR3's high byte the package's partition, of the 24 there are */
                if ((cr3_cr4 >> 24) >= 24) {
                        return false;
                }
        }

        record.argument = cr3_cr4;
        record.job_id = cr1_cr2;
        record.operation = operation;
        record.state = STATE_SUBMITTED;
        record.result = 0;
        WORK_REQUEST |= REQUEST_JOB;

        reply_with_record(SUBMIT_MESSAGE);

        return true;
}

/* AC, status: posts nothing */
bool
command_status(void)
{
        reply_with_record(0);

        return true;
}

/* AD, identity: the protocol version and the activations, then "DOSE" */
bool
command_identity(void)
{
        reply.post = 0;
        reply.cr1_cr2 = ((uint32_t)PROTOCOL_VERSION << 16) | record.activations;
        reply.cr3_cr4 = IDENTITY;

        return true;
}

/*
 * Whether the job is still its own, with interrupts masked: its request bit still set, the record
 * running, and no activation since it started. A 93 stop clears the request, an activation counts
 * itself and clears the record, and either ends the job with nothing published.
 */
static bool
job_owned(void)
{
        return (WORK_REQUEST & REQUEST_JOB) != 0 &&
            record.state == STATE_RUNNING &&
            record.activations == record.job_activations;
}

/* With job_owned() checked: the result and the state in the record, the request cleared, MPED */
static void
publish_owned(uint32_t result, uint8_t state)
{
        record.result = result;
        record.state = state;
        WORK_REQUEST &= ~REQUEST_JOB;
        INTERRUPT_FLAGS = FLAG_MPED;
}

static void
job_end(uint32_t result, uint8_t state)
{
        const uint32_t sr = interrupts_mask();

        if (job_owned()) {
                publish_owned(result, state);
        }

        interrupts_restore(sr);
}

/*
 * A module calls these by number through its import table, with the C convention: arguments from
 * r4, the result in r0, r8..r14 kept. The CD buffer's slots belong to the firmware, and these go
 * through its own routines, taking the sector lock that guards the slots around them.
 */

/* 1, slot_at: the slot at a position of a partition, or -1 */
static int32_t
service_slot_at(uint32_t partition, uint32_t position)
{
        sector_lock();
        const uint8_t slot = rom_slot_at(partition, position);
        sector_unlock();

        return (slot == 0xff) ? -1 : slot;
}

/*
 * 2, sector_data: where a slot's data starts, by the mode byte the drive wrote at +15, as 61 finds
 * it. Mode 1 data follows a 16-byte header, mode 2 a 24-byte one. A mode 2 form 2 sector (submode
 * bit 5 at +18) holds 2324 bytes, not a package's 2048, so it has none.
 */
static const uint8_t *
service_sector_data(uint32_t slot)
{
        const uint8_t *const at = rom_slot_address(slot);
        const int8_t mode = at[15];

        if (mode == 1) {
                return at + 16;
        }

        if (mode == 2 && (at[18] & 0x20) != 0) {
                return 0;
        }

        return at + 24;
}

/* 3, take: a free slot, its first 24 bytes cleared so it reads as a mode 0 sector, or -1 */
static int32_t
service_take(void)
{
        int32_t slot = rom_slot_take();

        if (slot == -1) {
                return -1;
        }

        slot &= 0xff;
        rom_slot_clear(rom_slot_address(slot), CLEAR_HEADER);

        return slot;
}

/* Back to the pool, under the sector lock */
static void
slot_back(uint32_t slot)
{
        sector_lock();
        rom_slot_return(slot);
        sector_unlock();
}

/*
 * 4, file: the 280 bytes after a taken slot's data cleared, then the slot filed through a filter
 * into its partition, where the Saturn can read it. Filed only while the job is its own: a stopped
 * job's slot goes back to the pool, and the module sees 0.
 */
static int32_t
service_file(uint32_t slot, uint32_t filter)
{
        rom_slot_clear(rom_slot_address(slot) + 24 + SECTOR_BYTES, CLEAR_TAIL);

        const uint32_t sr = interrupts_mask();

        if (!job_owned()) {
                interrupts_restore(sr);
                slot_back(slot);

                return 0;
        }

        FILED_FILTER = filter;
        FILED_SLOT = slot;
        interrupts_restore(sr);
        rom_slot_file();

        return 1;
}

/* 5, give_back: a slot from take, unused */
static int32_t
service_give_back(uint32_t slot)
{
        slot_back(slot);

        return 0;
}

/* Each service's address by its number, from 1 */
#define SERVICE_COUNT 5

static const uint32_t services[SERVICE_COUNT] = {
        (uint32_t)service_slot_at,
        (uint32_t)service_sector_data,
        (uint32_t)service_take,
        (uint32_t)service_file,
        (uint32_t)service_give_back
};

/*
 * A package is a module's image as it runs in the slot, from its header to its CRC, in the
 * sectors of one partition or in the card ROM. A load runs over several passes:
 * - Pass 0 finds the sectors and checks the header.
 * - Passes 1 to the sector count each take one sector into the CRC. A load from the card ROM has
 *   none of these.
 * - The pass after copies the image into the slot and installs it.
 * The installed module is left alone until the last, so a package that fails leaves it callable.
 */
struct package_header {
        uint32_t magic;                 /* "DOSM" */
        uint16_t unused;                /* 0 */
        uint16_t imports;               /* the import table's entries */
        uint32_t image;                 /* bytes, through the CRC */
        uint32_t bss;                   /* bytes after the image, cleared */
        uint32_t stack;                 /* bytes below the SP the entry is called with */
        uint32_t entry;
        uint32_t table;                 /* the import table */
        uint32_t zero;
};

#define HEADER_BYTES 32

static void
load_fail(uint32_t reason)
{
        job_end(reason << 24, STATE_FAILED);
}

/* Checks the header's sizes and addresses against the slot and the sectors found */
static uint32_t
load_check(const struct package_header *h)
{
        if (h->magic != MODULE_MAGIC || h->zero != 0) {
                return LOAD_NOT_PACKAGE;
        }

        /* The image: the header and the CRC at least, whole longwords, in the slot, exactly the
         * sectors found */
        if (h->image < HEADER_BYTES + 4 || (h->image & 3) != 0 || h->image > SLOT_BYTES ||
            (h->image + SECTOR_BYTES - 1) / SECTOR_BYTES != job.count) {
                return LOAD_BOUNDS;
        }

        /* The BSS: whole longwords, in the slot with the image */
        if ((h->bss & 3) != 0 || h->bss > SLOT_BYTES || h->image + h->bss > SLOT_BYTES) {
                return LOAD_BOUNDS;
        }

        if (h->stack > STACK_MOST) {
                return LOAD_BOUNDS;
        }

        /* The entry: an instruction's address, after the header and before the CRC */
        const uint32_t body = SLOT_START + HEADER_BYTES;
        const uint32_t crc_at = SLOT_START + h->image - 4;

        if ((h->entry & 1) != 0 || h->entry < body || h->entry >= crc_at) {
                return LOAD_BOUNDS;
        }

        /* The import table: aligned, after the header, ending at the CRC at the latest */
        if ((h->table & 3) != 0 || h->table < body || h->table > crc_at ||
            h->table + 4 * (uint32_t)h->imports > crc_at) {
                return LOAD_BOUNDS;
        }

        /* Each import a service. The image's byte o is byte o % 2048 of sector o / 2048. */
        for (uint32_t i = 0; i < h->imports; i++) {
                const uint32_t o = h->table - SLOT_START + 4 * i;
                const uint32_t number = *(const uint32_t *)(job.data[o / SECTOR_BYTES] +
                    o % SECTOR_BYTES);

                if (number - 1 >= SERVICE_COUNT) {
                        return LOAD_UNKNOWN_IMPORT;
                }
        }

        job.image = h->image;
        job.bss = h->bss;
        job.entry = h->entry;
        job.table = h->table;
        job.imports = h->imports;

        return 0;
}

/* Pass 0, the argument's high byte the partition: its sectors, then the header */
static void
load_start(uint32_t argument)
{
        job.partition = argument >> 24;
        job.count = 0;
        job.done = 0;
        job.crc = 0xffffffff;

        for (;;) {
                const int32_t slot = service_slot_at(job.partition, job.count);

                if (slot < 0) {
                        break;
                }

                if (job.count == PACKAGE_MOST) {
                        load_fail(LOAD_NO_SECTORS);
                        return;
                }

                const uint8_t *const data = service_sector_data(slot);

                if (data == 0) {
                        load_fail(LOAD_FORM2);
                        return;
                }

                job.data[job.count++] = data;
        }

        if (job.count == 0) {
                load_fail(LOAD_NO_SECTORS);
                return;
        }

        const uint32_t reason = load_check((const struct package_header *)job.data[0]);

        if (reason != 0) {
                load_fail(reason);
                return;
        }

        job.pass++;
}

/*
 * Pass 0 of a job on a library handle whose module is not the one in the slot. The package lies
 * whole in the card ROM, so its "sectors" are simply its 2048-byte pieces there, and the rest of
 * the load runs as from a partition: the same checks, then the install. The CRC passes are
 * skipped, since the card ROM cannot change: the passes go on from the pieces' count, so the next
 * one installs. Its last pass runs the module's own first pass next.
 */
static void
library_start(uint8_t handle)
{
        const uint8_t *const package = (const uint8_t *)CARD_ROM +
            LIBRARY->offsets[handle - LIBRARY_FIRST];
        const struct package_header *const h = (const struct package_header *)package;

        if (h->image > SLOT_BYTES) {
                load_fail(LOAD_BOUNDS);
                return;
        }

        job.count = (h->image + SECTOR_BYTES - 1) / SECTOR_BYTES;

        for (uint32_t i = 0; i < job.count; i++) {
                job.data[i] = package + i * SECTOR_BYTES;
        }

        const uint32_t reason = load_check(h);

        if (reason != 0) {
                load_fail(reason);
                return;
        }

        job.pass = job.count + 1;
}

/*
 * CRC-32 as zlib computes it, reflected, four bits at a time: 16 longwords of table instead of
 * the usual 256, at twice the steps.
 */
static const uint32_t crc_nibbles[16] = {
        0x00000000, 0x1db71064, 0x3b6e20c8, 0x26d930ac, 0x76dc4190, 0x6b6b51f4, 0x4db26158,
        0x5005713c, 0xedb88320, 0xf00f9344, 0xd6d6a3e8, 0xcb61b38c, 0x9b64c2b0, 0x86d3d2d4,
        0xa00ae278, 0xbdbdf21c
};

static uint32_t
crc_update(uint32_t crc, const uint8_t *data, uint32_t bytes)
{
        while (bytes-- != 0) {
                crc ^= *data++;
                crc = (crc >> 4) ^ crc_nibbles[crc & 15];
                crc = (crc >> 4) ^ crc_nibbles[crc & 15];
        }

        return crc;
}

/*
 * The install, while the job is its own. The module is forgotten first, so a stop from here on
 * leaves none. Then the image is copied sector by sector, the BSS cleared and each import's
 * service number replaced by the service's address, all by the sizes the first pass checked, so
 * nothing is written outside the slot even if the sectors have changed since.
 */
static void
load_install(void)
{
        uint32_t sr = interrupts_mask();

        if (!job_owned()) {
                interrupts_restore(sr);
                return;
        }

        installed.handle = 0;
        interrupts_restore(sr);

        uint32_t *to = (uint32_t *)SLOT_START;

        for (uint32_t i = 0, left = job.image; left != 0; i++) {
                const uint32_t bytes = (left > SECTOR_BYTES) ? SECTOR_BYTES : left;
                const uint32_t *from = (const uint32_t *)job.data[i];

                for (uint32_t n = bytes / 4; n != 0; n--) {
                        *to++ = *from++;
                }

                left -= bytes;
        }

        for (uint32_t n = job.bss / 4; n != 0; n--) {
                *to++ = 0;
        }

        uint32_t *const imports = (uint32_t *)job.table;

        for (uint32_t i = 0; i < job.imports; i++) {
                imports[i] = services[imports[i] - 1];
        }

        /*
         * Installed, while the job is still its own. A load from a partition takes the next handle
         * and publishes it. A library module takes its own handle, and the job goes on as the
         * module's, from its pass 0.
         */
        sr = interrupts_mask();

        if (job_owned()) {
                installed.entry = job.entry;

                if (record.operation == OPERATION_LOAD) {
                        const uint8_t handle = installed.next_handle;

                        installed.handle = handle;
                        installed.next_handle = (handle == HANDLE_LAST) ? HANDLE_FIRST : handle + 1;
                        publish_owned(handle, STATE_DONE);
                } else {
                        installed.handle = record.operation;
                        job.pass = 0;
                }
        }

        interrupts_restore(sr);
}

/* Each later pass: pass n of the sector count takes sector n - 1 into the CRC, the next installs */
static void
load_pass(void)
{
        if (job.pass > job.count) {
                load_install();
                return;
        }

        /* Up to a sector of the bytes before the CRC that are left */
        const uint8_t *const data = job.data[job.pass - 1];
        uint32_t bytes = job.image - 4 - job.done;

        if (bytes > SECTOR_BYTES) {
                bytes = SECTOR_BYTES;
        }

        job.done += bytes;
        job.crc = crc_update(job.crc, data, bytes);

        /* After the last sector's bytes comes the CRC, the image's last longword */
        if (job.pass == job.count && ~job.crc != *(const uint32_t *)(data + bytes)) {
                load_fail(LOAD_CRC);
                return;
        }

        job.pass++;
}

/*
 * A pass of the module's job: its entry with the pass, the argument and the result word. It
 * returns 0 to be called again, 1 done or 2 failed, and any other value counts as failed.
 */
static void
module_pass(void)
{
        const int32_t status = module_call(job.pass, record.argument, &job.result,
            installed.entry);

        if (status == 0) {
                job.pass++;
                return;
        }

        job_end(job.result, (status == 1) ? STATE_DONE : STATE_FAILED);
}

/* Operation 2, unload: the module forgotten, while the job is its own */
static void
unload(void)
{
        const uint32_t sr = interrupts_mask();

        if (job_owned()) {
                installed.handle = 0;
                publish_owned(0, STATE_DONE);
        }

        interrupts_restore(sr);
}

/*
 * Entry 54, from task 9's card work loop while bit 0 of the work request is set. Each call is one
 * pass, the first the job's start, and the loop calls again until the job's end clears the bit.
 * The record is read with interrupts masked, so no AB or activation comes between reading it and
 * acting on it.
 */
void
card_run(void)
{
        const uint32_t sr = interrupts_mask();
        const uint8_t state = record.state;
        const uint8_t operation = record.operation;

        if (state == STATE_SUBMITTED) {
                /* The start: the activations it runs in, then running */
                const uint32_t argument = record.argument;

                record.job_activations = record.activations;
                record.state = STATE_RUNNING;
                interrupts_restore(sr);

                job.pass = 0;

                switch (operation) {
                case OPERATION_PING:
                        job_end(~argument, STATE_DONE);
                        break;
                case OPERATION_UNLOAD:
                        unload();
                        break;
                case OPERATION_LOAD:
                        load_start(argument);
                        break;
                default:
                        /* A module's job: a library module not in the slot is loaded first */
                        if (operation == installed.handle) {
                                module_pass();
                        } else {
                                library_start(operation);
                        }
                        break;
                }

                return;
        }

        if (state == STATE_RUNNING) {
                interrupts_restore(sr);

                /* A load, or a library module's load that its install has not ended yet */
                if (operation == OPERATION_LOAD || operation != installed.handle) {
                        load_pass();
                } else {
                        module_pass();
                }

                return;
        }

        /* No job submitted or running: a request bit left from elsewhere */
        WORK_REQUEST &= ~REQUEST_JOB;
        interrupts_restore(sr);
}
