#include <yaul.h>

#include "sh1-client.h"

/* The CD block's host registers and data port, through the cache-through area. A longword read of
 * the data port here takes two words. */
#define CD_HIRQ (*(volatile uint16_t *)0x25890008UL)
#define CD_CR(n) (*(volatile uint16_t *)(0x25890018UL + 4 * (n)))
#define CD_DATA 0x25818000UL

/* Polls of HIRQ before a wait gives up, some seconds. */
#define WAIT_POLLS 0x1000000UL

/* 51s before a disc read gives up, some seconds. */
#define READ_POLLS 0x40000UL

uint16_t sh1_answer[4];
uint8_t sh1_protocol;

/*
 * Waits for the SH-1 to raise any of the flags. The SH-1 works at its own pace, so everything the
 * SH-2 asks of it ends with a wait like this one. The client polls rather than use the CD block's
 * interrupt, which keeps every command in the main program and out of interrupt handlers. A wait
 * that runs out returns false: the CD block did not answer.
 */
static bool
wait_flags(uint16_t flags)
{
        for (uint32_t i = 0; i < WAIT_POLLS; i++) {
                if ((CD_HIRQ & flags) != 0) {
                        return true;
                }
        }

        return false;
}

/* Whether the last answer's status takes the command: not rejected, not told to wait. */
static bool
accepted(void)
{
        const uint8_t status = sh1_answer[0] >> 8;

        return (status != SH1_STATUS_REJECT) && ((status & SH1_STATUS_WAIT) == 0);
}

/*
 * One command and its answer, the step every other function here is built from:
 *
 * 1. Clear CMOK, and the flag the command will raise when its work is done, so that a flag left
 *    from an earlier command cannot pass for this one's.
 * 2. Write CR1 to CR4. The CR4 write hands the command to the SH-1.
 * 3. Wait for CMOK: the answer is in the same four registers.
 * 4. For a command whose work goes on after its answer (a transfer, a card load), wait for that
 *    flag too.
 */
bool
sh1_command(uint16_t cr1, uint16_t cr2, uint16_t cr3, uint16_t cr4, uint16_t wait)
{
        CD_HIRQ = (uint16_t)~(SH1_CMOK | wait);
        CD_CR(0) = cr1;
        CD_CR(1) = cr2;
        CD_CR(2) = cr3;
        CD_CR(3) = cr4;

        if (!wait_flags(SH1_CMOK)) {
                return false;
        }

        for (uint32_t i = 0; i < 4; i++) {
                sh1_answer[i] = CD_CR(i);
        }

        if (wait != 0) {
                return accepted() && wait_flags(wait);
        }

        return true;
}

bool
sh1_reset_selector(void)
{
        return sh1_command(0x48FC, 0x0000, 0x0000, 0x0000, SH1_ESEL);
}

/*
 * E0 with subcode 1 is a stock command, the MPEG card's authentication: the CD block checks the
 * card in the MPEG slot, decodes the SH-1 code in the card's ROM into its RAM and raises MPED when
 * done. The demo's card passes the same checks, so the same command loads our firmware. E0 itself
 * answers before the load has run, so its outcome is read from two other commands.
 */
bool
sh1_card_load(void)
{
        /* E1's CR2 low byte is 2 once the checks pass, and 01's MPEG version 0 after a failed load */
        return sh1_command(0xE000, 0x0001, 0x0000, 0x0000, SH1_MPED) &&
            sh1_command(0xE100, 0x0001, 0x0000, 0x0000, 0) && accepted() &&
            ((sh1_answer[1] & 0xFF) == 2) && sh1_command(0x0100, 0x0000, 0x0000, 0x0000, 0) &&
            accepted() && ((sh1_answer[2] & 0xFF) != 0);
}

bool
sh1_card_on(void)
{
        return sh1_command(0x9300, 0x0001, 0x0000, 0x0000, SH1_MPED);
}

/* CHCRn, DMA channel n's control register: TE, bit 1, is set once its count runs out. */
#define DMAC_CHCR(n) (*(volatile uint32_t *)(0xFFFFFF8CUL + 0x10UL * (n)))
#define DMAC_TE 0x00000002UL

/* The transfer open since its start, until sh1_transfer_end(): 0 words when none. */
static struct {
        void *data;
        uint32_t bytes;
        uint32_t words;
        bool to_host;
} open_transfer;

/*
 * Starts moving bytes between data and the data port in longwords by the DMA controller, and
 * returns while it runs.
 *
 * The DMA controller is part of the SH-2: given a source, a destination and a count, it copies by
 * itself while the CPU goes on with the program. Here one end is the data port, a single address
 * that stays fixed, and the other end is memory, whose address moves on with each longword. In
 * cycle steal mode the controller takes the bus for one longword at a time, between the CPU's own
 * accesses, where burst mode would hold the CPU until the whole count is done.
 */
static void
transfer_start(void *data, uint32_t bytes, bool to_host)
{
        const cpu_dmac_cfg_t cfg = {
                .channel = SH1_DMAC_CHANNEL,
                .src_mode = to_host ? CPU_DMAC_SOURCE_FIXED : CPU_DMAC_SOURCE_INCREMENT,
                .dst_mode = to_host ? CPU_DMAC_DESTINATION_INCREMENT : CPU_DMAC_DESTINATION_FIXED,
                .src = to_host ? CD_DATA : (uint32_t)data,
                .dst = to_host ? (uint32_t)data : CD_DATA,
                .len = bytes,
                .stride = CPU_DMAC_STRIDE_4_BYTES,
                .bus_mode = CPU_DMAC_BUS_MODE_CYCLE_STEAL,
                .ihr = NULL,
                .ihr_work = NULL
        };

        cpu_dmac_channel_config_set(&cfg);
        cpu_dmac_enable();
        cpu_dmac_channel_start(SH1_DMAC_CHANNEL);

        open_transfer.data = data;
        open_transfer.bytes = bytes;
        open_transfer.words = bytes / 2;
        open_transfer.to_host = to_host;
}

/*
 * After a transfer: DRDY and EHST cleared. The CD block only sets its flags and the host clears
 * them, so a DRDY left set would tell the next transfer's reader, libyaul's sector reads among
 * them, that its data is ready before the SH-1 has started it.
 */
static void
clear_transfer_flags(void)
{
        CD_HIRQ = (uint16_t)~(SH1_DRDY | SH1_EHST);
}

bool
sh1_transfer_open(void)
{
        return open_transfer.words != 0;
}

bool
sh1_transfer_busy(void)
{
        return sh1_transfer_open() && ((DMAC_CHCR(SH1_DMAC_CHANNEL) & DMAC_TE) == 0);
}

/* 06 ends the open transfer: its answer, then EHST. True when the answer's word count, CR1 low byte
 * over CR2, is every word programmed. A transfer error answers ffffff. */
bool
sh1_transfer_end(void)
{
        const uint32_t words = open_transfer.words;

        if (words == 0) {
                return false;
        }

        cpu_dmac_channel_wait(SH1_DMAC_CHANNEL);

        /* The DMA controller wrote memory behind the cache, so lines of it cached before are stale */
        if (open_transfer.to_host) {
                cpu_cache_area_purge(open_transfer.data, open_transfer.bytes);
        }

        open_transfer.words = 0;

        CD_HIRQ = (uint16_t)~(SH1_CMOK | SH1_EHST);

        const bool ended = sh1_command(0x0600, 0x0000, 0x0000, 0x0000, 0) && wait_flags(SH1_EHST);

        clear_transfer_flags();

        return ended && (((((uint32_t)sh1_answer[0] & 0xFF) << 16) | sh1_answer[1]) == words);
}

/* 06 after a data command whose transfer did not run. An accepted answer has opened a transfer, and
 * until 06 every other data command is told to wait. With none open 06 answers ffffff and does
 * nothing. */
static void
abandon_transfer(void)
{
        (void)sh1_command(0x0600, 0x0000, 0x0000, 0x0000, 0);

        clear_transfer_flags();
}

/*
 * A put or a get is a command and then a transfer. The command says which sectors, and the SH-1
 * answers and raises DRDY once the data port is ready for them. From there the SH-1 feeds or
 * drains the port while the SH-2's DMA controller does the same from its side, and 06 closes the
 * transfer, in sh1_transfer_end().
 */
bool
sh1_put_start(uint8_t filter, uint16_t count, const void *data)
{
        /* One transfer at a time: a 06 here would end the one open */
        if (sh1_transfer_open()) {
                return false;
        }

        /* No free slots gives EHST without DRDY */
        if (!sh1_command(0x6400, 0x0000, (uint16_t)(filter << 8), count, SH1_DRDY | SH1_EHST) ||
            ((CD_HIRQ & SH1_DRDY) == 0)) {
                abandon_transfer();
                return false;
        }

        transfer_start((void *)data, (uint32_t)count * SH1_SECTOR_BYTES, false);

        return true;
}

bool
sh1_get_start(uint8_t partition, uint16_t position, uint16_t count, void *data, bool take)
{
        /* 63 is 61 that also deletes the sectors once the transfer ends */
        const uint16_t command = take ? 0x6300 : 0x6100;

        if (sh1_transfer_open()) {
                return false;
        }

        if (!sh1_command(command, position, (uint16_t)(partition << 8), count, SH1_DRDY)) {
                abandon_transfer();
                return false;
        }

        transfer_start(data, (uint32_t)count * SH1_SECTOR_BYTES, true);

        return true;
}

bool
sh1_put(uint8_t filter, uint16_t count, const void *data)
{
        return sh1_put_start(filter, count, data) && sh1_transfer_end();
}

bool
sh1_get(uint8_t partition, uint16_t position, uint16_t count, void *data)
{
        return sh1_get_start(partition, position, count, data, false) && sh1_transfer_end();
}

bool
sh1_delete(uint8_t partition, uint16_t position, uint16_t count)
{
        return sh1_command(0x6200, position, (uint16_t)(partition << 8), count, SH1_EHST);
}

bool
sh1_sectors(uint8_t partition, uint16_t *count)
{
        if (!sh1_command(0x5100, 0x0000, (uint16_t)(partition << 8), 0x0000, 0) || !accepted()) {
                return false;
        }

        *count = sh1_answer[3];

        return true;
}

bool
sh1_connect(uint8_t filter)
{
        /* ESEL: the connection is made */
        return sh1_command(0x3000, 0x0000, (uint16_t)(filter << 8), 0x0000, SH1_ESEL);
}

void
sh1_disconnect(void)
{
        (void)sh1_command(0x3000, 0x0000, 0xFF00, 0x0000, SH1_ESEL);
}

bool
sh1_play(uint32_t fad, uint16_t count)
{
        /* Bit 23 of each position marks a frame address, and an end so marked is a sector count.
         * CR3's high byte is the play mode, 0 for a single play. */
        return sh1_command((uint16_t)(0x1080 | ((fad >> 16) & 0x7F)), (uint16_t)fad, 0x0080, count,
                   0) && accepted();
}

bool
sh1_move(uint8_t partition, uint16_t position, uint16_t count, uint8_t filter)
{
        return sh1_command((uint16_t)(0x6600 | filter), position, (uint16_t)(partition << 8), count,
            SH1_ECPY);
}

bool
sh1_read(uint8_t filter, uint32_t fad, uint16_t count)
{
        /* The filter's partition, as sh1_reset_selector() connects them */
        const uint8_t partition = filter;
        uint16_t before;

        if (!sh1_sectors(partition, &before) || !sh1_connect(filter)) {
                return false;
        }

        if (!sh1_play(fad, count)) {
                sh1_disconnect();
                return false;
        }

        for (uint32_t i = 0; i < READ_POLLS; i++) {
                uint16_t now;

                if (!sh1_sectors(partition, &now)) {
                        break;
                }

                if ((uint16_t)(now - before) >= count) {
                        return true;
                }
        }

        sh1_disconnect();

        return false;
}

/*
 * From here on, the card firmware's own commands. On a console without the card, or with a stock
 * MPEG card, AD does not answer "DOSE", so a program asks it first and uses the SH-1 only when it
 * does. The version tells which of the firmware's features the client may use.
 */
bool
sh1_identity(uint16_t *activations)
{
        sh1_protocol = 0;

        if (!sh1_command(0xAD00, 0x0000, 0x0000, 0x0000, 0) || !accepted() ||
            (sh1_answer[2] != 0x444F) || (sh1_answer[3] != 0x5345)) {
                return false;
        }

        const uint8_t version = sh1_answer[0] & 0xFF;

        if ((version < SH1_PROTOCOL_OLDEST) || (version > SH1_PROTOCOL)) {
                return false;
        }

        sh1_protocol = version;
        *activations = sh1_answer[1];

        return true;
}

bool
sh1_submit(uint8_t operation, uint16_t id, uint32_t argument)
{
        /* An MPED from before belongs to another request */
        CD_HIRQ = (uint16_t)~SH1_MPED;

        return sh1_command((uint16_t)(0xAB00 | operation), id, (uint16_t)(argument >> 16),
            (uint16_t)argument, 0) && accepted();
}

bool
sh1_status(sh1_job_t *job)
{
        if (!sh1_command(0xAC00, 0x0000, 0x0000, 0x0000, 0) || !accepted()) {
                return false;
        }

        job->state = sh1_answer[0] & 0xFF;
        job->id = sh1_answer[1];
        job->result = ((uint32_t)sh1_answer[2] << 16) | sh1_answer[3];

        return true;
}

/*
 * A job runs on the SH-1 while the SH-2 does something else, so its end is found in two steps.
 * MPED says "something ended", and it is the MPEG card's one flag for that, raised by 93 and by a
 * card load too. AC then gives the job's record, and the job ID in it tells this job's end from a
 * stale one.
 */
bool
sh1_wait(uint16_t id, sh1_job_t *job)
{
        while (true) {
                const bool flagged = wait_flags(SH1_MPED);

                /* Cleared before AC is read, so that an end after the read sets it again */
                CD_HIRQ = (uint16_t)~SH1_MPED;

                if (!sh1_status(job) || (job->id != id) || (job->state == SH1_JOB_NONE)) {
                        return false;
                }

                if ((job->state == SH1_JOB_DONE) || (job->state == SH1_JOB_FAILED)) {
                        return true;
                }

                /* An MPED that ended another request waits again, a timeout gives up */
                if (!flagged) {
                        return false;
                }
        }
}

/*
 * A module reaches the SH-1 the way any data does: as sectors in a partition. The load job then
 * checks the package there and copies it into the module slot, so no code crosses to the SH-1 by
 * any path the stock firmware does not already have.
 */
bool
sh1_module_load(uint8_t partition, const void *package, uint16_t sectors, uint16_t id,
    sh1_job_t *job)
{
        job->state = SH1_JOB_NONE;

        if (!sh1_put(partition, sectors, package) ||
            !sh1_submit(SH1_OP_LOAD, id, (uint32_t)partition << 24)) {
                (void)sh1_delete(partition, 0, 0xFFFF);
                return false;
        }

        if (!sh1_wait(id, job)) {
                return false;
        }

        return sh1_delete(partition, 0, 0xFFFF) && (job->state == SH1_JOB_DONE);
}

sh1_library_t
sh1_library_check(uint8_t handle, uint16_t id, sh1_job_t *job)
{
        job->state = SH1_JOB_NONE;

        /* Protocol 1 takes only a partition there, and would reject the load */
        if (sh1_protocol < SH1_PROTOCOL_LIBRARY_CHECK) {
                return SH1_LIBRARY_UNCHECKED;
        }

        /* Below the library's handles the byte would name a partition, and load from it */
        if (handle < SH1_LIBRARY_FIRST) {
                return SH1_LIBRARY_MISSING;
        }

        /* An MPED from before belongs to another request */
        CD_HIRQ = (uint16_t)~SH1_MPED;

        /* A CD block that does not answer is no finding about the library */
        if (!sh1_command((uint16_t)(0xAB00 | SH1_OP_LOAD), id, (uint16_t)(handle << 8),
                (uint16_t)SH1_LOAD_CHECK, 0)) {
                return SH1_LIBRARY_UNCHECKED;
        }

        /* Rejected: the library has no module on the handle */
        if ((sh1_answer[0] >> 8) == SH1_STATUS_REJECT) {
                return SH1_LIBRARY_MISSING;
        }

        /* Told to wait: no finding either */
        if (!accepted()) {
                return SH1_LIBRARY_UNCHECKED;
        }

        if (!sh1_wait(id, job)) {
                return SH1_LIBRARY_UNCHECKED;
        }

        return ((job->state == SH1_JOB_DONE) && (job->result == handle)) ? SH1_LIBRARY_OK :
            SH1_LIBRARY_BAD;
}
