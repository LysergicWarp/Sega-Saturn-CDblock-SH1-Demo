#include <yaul.h>

#include <bcl.h>

#include <sh1-client.h>

#include "dispatch.h"

/*
 * The slave's mailbox.
 *
 * Both SH-2s have their own cache, and neither sees what is in the other's. Memory written by one
 * can sit stale in the other's cache. So the mailbox is __uncached: libyaul links it at its
 * cache-through address (0x20000000 up), where every read and write goes to memory.
 *
 * - job: the job the master hands over, NULL when there is none.
 * - ready: set by the slave each time it runs its entry, so the master knows it listens.
 */
static struct {
        dispatch_job_t *volatile job;
        volatile uint32_t ready;
} slave_box __uncached;

/*
 * The SH-1's side.
 *
 * The CD block keeps sectors in partitions, filled through filters. The client's selector reset
 * connects filter n to partition n, so the input put, moved or read from the disc through filter
 * SH1_INPUT lands in partition SH1_INPUT and the module files its output through filter
 * SH1_OUTPUT into partition SH1_OUTPUT.
 * Partition 0 is left to libyaul's own CD reads, partitions from DISPATCH_FIRST_FREE to the caller.
 */
#define SH1_INPUT 1
#define SH1_OUTPUT 2

/* Polls of the slave's ready flag before dispatch_init() gives up on it. */
#define SLAVE_POLLS 0x100000UL

/*
 * Where each processor's job is. A job runs through some of these in order:
 *
 * - LOADING: its input crosses the data port from a partition into SH-2 memory.
 * - WORKING: a processor decodes it.
 * - RETURNING: the SH-1's output crosses the data port back into SH-2 memory.
 *
 * During LOADING and RETURNING the job owns the data port's one transfer, and no other command
 * goes to the CD block until it ends.
 */
typedef enum {
        PHASE_LOADING,
        PHASE_WORKING,
        PHASE_RETURNING
} phase_t;

static bool available[DISPATCH_PROCESSORS];
static dispatch_job_t *current[DISPATCH_PROCESSORS];
static phase_t phase[DISPATCH_PROCESSORS];

/* The processor whose job owns the open transfer, while sh1_transfer_open(). */
static dispatch_processor_t port_owner;

/* The decompress module's handle, and the ID of the SH-1's last job. */
static uint8_t sh1_handle;
static uint16_t sh1_id;

static uint32_t
sectors_of(uint32_t bytes)
{
        return (bytes + SH1_SECTOR_BYTES - 1) / SH1_SECTOR_BYTES;
}

/* An SH-2's job: the caller's function, or the decode. */
static void
decode(dispatch_job_t *job)
{
        if (job->call != NULL) {
                job->call(job->work);
                return;
        }

        bcl_prs_decompress(job->input, job->output);
}

/*
 * Runs on the slave each time the master notifies it (libyaul's polling mode: the slave spins on
 * its notify flag and calls this).
 *
 * The slave's cache can still hold lines of memory the master has written since, the job record
 * and its input among them, so the slave purges its cache first. Its own writes need nothing: the
 * SH-2's cache is write-through, so the output goes to memory as it is written.
 */
static void
slave_entry(void)
{
        slave_box.ready = 1;

        dispatch_job_t *const job = slave_box.job;

        if (job == NULL) {
                return;
        }

        /* Taken: a second notify for the same job finds nothing to do */
        slave_box.job = NULL;

        cpu_cache_purge();
        decode(job);

        /* The record is the master's, so the state goes through the cache-through alias */
        *(volatile dispatch_state_t *)((uintptr_t)&job->state | CPU_CACHE_THROUGH) = DISPATCH_DONE;
}

/*
 * Starts the slave and waits for it to answer.
 *
 * libyaul's cpu_dual_comm_mode_set() (re)starts the slave and returns without waiting for it. A
 * notify sent before the slave reaches its loop is lost, so the master keeps notifying until the
 * slave has set ready once.
 */
static bool
slave_start(void)
{
        slave_box.job = NULL;
        slave_box.ready = 0;

        cpu_dual_comm_mode_set(CPU_DUAL_ENTRY_POLLING);
        cpu_dual_slave_set(slave_entry);

        for (uint32_t i = 0; i < SLAVE_POLLS; i++) {
                cpu_dual_slave_notify();

                if (slave_box.ready != 0) {
                        return true;
                }
        }

        return false;
}

/*
 * Brings the SH-1 up: the card firmware loaded from the MPEG card and turned on, its identity
 * checked, every partition emptied with the filters connected as above, and the decompress module
 * uploaded. The module's handle names it in every job.
 *
 * Built with DISPATCH_FIRMWARE_FROM_DISC, the card load is left out: a disc extension has put the
 * firmware in place already, and E0's card load would run the card checks against an empty slot,
 * which fail and clear it.
 */
static bool
sh1_start(const void *package, uint32_t package_bytes)
{
        uint16_t activations;
        sh1_job_t job;

#ifndef DISPATCH_FIRMWARE_FROM_DISC
        if (!sh1_card_load()) {
                return false;
        }
#endif

        if (!sh1_card_on() || !sh1_identity(&activations) || !sh1_reset_selector()) {
                return false;
        }

        if (!sh1_module_load(SH1_INPUT, package, (uint16_t)sectors_of(package_bytes), ++sh1_id,
                &job)) {
                return false;
        }

        sh1_handle = (uint8_t)job.result;

        return true;
}

void
dispatch_init(const void *package, uint32_t package_bytes)
{
        available[DISPATCH_MASTER] = true;
        available[DISPATCH_SLAVE] = slave_start();
        available[DISPATCH_SH1] = sh1_start(package, package_bytes);
}

bool
dispatch_available(dispatch_processor_t processor)
{
        return (processor < DISPATCH_PROCESSORS) && available[processor];
}

bool
dispatch_port_busy(void)
{
        return sh1_transfer_open();
}

/*
 * The SH-1's job: the stream into partition SH1_INPUT, then AB to the module's handle with the
 * argument it takes: CR3 the input partition and the output filter, CR4 the most output sectors.
 * AB returns at once, the module runs on the SH-1 while the master goes on.
 *
 * The stream gets there one of three ways, none through the SH-2 but the put:
 * - from another partition, moved inside the CD block (66),
 * - from the disc, the drive filing it straight into the partition,
 * - from SH-2 memory, put through the data port (64).
 */
static bool
sh1_submit_job(dispatch_job_t *job)
{
        const uint32_t limit = sectors_of(job->output_bytes);
        const uint32_t argument =
            ((uint32_t)SH1_INPUT << 24) | ((uint32_t)SH1_OUTPUT << 16) | limit;

        const uint16_t sectors = (uint16_t)sectors_of(job->input_bytes);

        bool filled;

        if (job->input_partition != 0) {
                filled = sh1_move(job->input_partition, 0, sectors, SH1_INPUT);
        } else if (job->input_fad != 0) {
                filled = sh1_read(SH1_INPUT, job->input_fad, sectors);
        } else {
                filled = sh1_put(SH1_INPUT, sectors, job->input);
        }

        if (!filled) {
                (void)sh1_delete(SH1_INPUT, 0, 0xFFFF);
                return false;
        }

        if (!sh1_submit(sh1_handle, ++sh1_id, argument)) {
                (void)sh1_delete(SH1_INPUT, 0, 0xFFFF);
                return false;
        }

        return true;
}

/* Hands a job whose input is in SH-2 memory to an SH-2: decoded right here on the master, through
 * the mailbox on the slave. */
static void
sh2_work(dispatch_processor_t processor, dispatch_job_t *job)
{
        if (processor == DISPATCH_MASTER) {
                decode(job);
                job->state = DISPATCH_DONE;
                current[processor] = NULL;
                return;
        }

        phase[processor] = PHASE_WORKING;
        slave_box.job = job;
        cpu_dual_slave_notify();
}

bool
dispatch_submit(dispatch_processor_t processor, dispatch_job_t *job)
{
        if (!dispatch_available(processor)) {
                job->state = DISPATCH_FAILED;
                return false;
        }

        if (current[processor] != NULL) {
                /* Busy. A job already running keeps its state, only another job is refused */
                if (current[processor] != job) {
                        job->state = DISPATCH_FAILED;
                }
                return false;
        }

        /* The SH-1 runs decodes only */
        if ((processor == DISPATCH_SH1) && (job->call != NULL)) {
                job->state = DISPATCH_FAILED;
                return false;
        }

        /* Every path but an SH-2 job from memory talks to the CD block, which waits for the open
         * transfer to end */
        const bool from_memory =
            (job->call != NULL) || ((job->input_partition == 0) && (job->input_fad == 0));

        if (dispatch_port_busy() && ((processor == DISPATCH_SH1) || !from_memory)) {
                job->state = DISPATCH_FAILED;
                return false;
        }

        job->state = DISPATCH_RUNNING;
        current[processor] = job;

        if (processor == DISPATCH_SH1) {
                if (!sh1_submit_job(job)) {
                        job->state = DISPATCH_FAILED;
                        current[processor] = NULL;
                        return false;
                }
                phase[processor] = PHASE_WORKING;
                return true;
        }

        /* An SH-2 decodes from its own memory, so a stream in a partition crosses the data port
         * first. The transfer runs by DMA while the caller goes on, and dispatch_poll() ends it. */
        if ((job->call == NULL) && (job->input_partition != 0)) {
                if (!sh1_get_start(job->input_partition, 0, (uint16_t)sectors_of(job->input_bytes),
                        job->input, true)) {
                        job->state = DISPATCH_FAILED;
                        current[processor] = NULL;
                        return false;
                }
                port_owner = processor;
                phase[processor] = PHASE_LOADING;
                return true;
        }

        /* A stream on the disc: libyaul reads it through the CD block's partition 0 and the data
         * port into input, before this returns */
        if ((job->call == NULL) && (job->input_fad != 0) &&
            (cd_block_sectors_read(job->input_fad, job->input, job->input_bytes) != 0)) {
                job->state = DISPATCH_FAILED;
                current[processor] = NULL;
                return false;
        }

        sh2_work(processor, job);

        return true;
}

/* An SH-2 job's input transfer: still running, or ended and the job handed on. */
static dispatch_state_t
loading_poll(dispatch_processor_t processor, dispatch_job_t *job)
{
        if (sh1_transfer_busy()) {
                return DISPATCH_RUNNING;
        }

        if (!sh1_transfer_end()) {
                return DISPATCH_FAILED;
        }

        sh2_work(processor, job);

        return job->state;
}

/*
 * The slave's job, seen from the master. Its state is read through the cache-through alias, since
 * the slave wrote it to memory behind the master's cache. Once it is done the master purges the
 * output's lines from its own cache, for the same reason: lines of the buffer it read before the
 * job would still hold the old bytes.
 *
 * One notify is enough once the slave has answered at the start. libyaul's slave loop clears the
 * notify flag before it calls the entry, so a notify that comes during an entry brings one more
 * call, and that call finds the job in the mailbox.
 */
static dispatch_state_t
slave_poll(dispatch_job_t *job)
{
        const dispatch_state_t state =
            *(volatile dispatch_state_t *)((uintptr_t)&job->state | CPU_CACHE_THROUGH);

        if (state != DISPATCH_DONE) {
                return DISPATCH_RUNNING;
        }

        /* A call's output is the caller's to purge, as only it knows where the call wrote */
        if (job->call == NULL) {
                cpu_cache_area_purge(job->output, (job->output_bytes + 15) & ~15UL);
        }

        return DISPATCH_DONE;
}

/* The SH-1's partitions emptied for its next job. */
static bool
sh1_clear(void)
{
        const bool input = sh1_delete(SH1_INPUT, 0, 0xFFFF);
        const bool output = sh1_delete(SH1_OUTPUT, 0, 0xFFFF);

        return input && output;
}

/*
 * The SH-1's job, seen from the master. AC gives the job's record: its ID, its state and, once
 * done, the bytes it decoded. MPED, the CD block's flag for a finished card request, is only a hint
 * (it also ends other requests), so the record is what counts.
 *
 * A done job's output then crosses the data port back by DMA (63, which also deletes it), while the
 * caller goes on. The poll after the transfer ends it and empties the input partition.
 */
static dispatch_state_t
sh1_poll(dispatch_job_t *job)
{
        if (phase[DISPATCH_SH1] == PHASE_RETURNING) {
                if (sh1_transfer_busy()) {
                        return DISPATCH_RUNNING;
                }

                /* A copy that failed may have left its sectors, which 63 deletes only at its end,
                 * so both partitions are emptied for the next job */
                if (!sh1_transfer_end()) {
                        (void)sh1_clear();
                        return DISPATCH_FAILED;
                }

                return sh1_delete(SH1_INPUT, 0, 0xFFFF) ? DISPATCH_DONE : DISPATCH_FAILED;
        }

        sh1_job_t record;

        if (!sh1_status(&record)) {
                /* No answer, so the job's state is unknown: it may still be running and filing
                 * sectors. The SH-1 takes no more jobs, as the next would share its partitions */
                available[DISPATCH_SH1] = false;
                return DISPATCH_FAILED;
        }

        const bool ours = (record.id == sh1_id);

        if (ours && ((record.state == SH1_JOB_SUBMITTED) || (record.state == SH1_JOB_RUNNING))) {
                return DISPATCH_RUNNING;
        }

        if (ours && (record.state == SH1_JOB_DONE) && (record.result == job->output_bytes) &&
            sh1_get_start(SH1_OUTPUT, 0, (uint16_t)sectors_of(job->output_bytes), job->output,
                true)) {
                port_owner = DISPATCH_SH1;
                phase[DISPATCH_SH1] = PHASE_RETURNING;
                return DISPATCH_RUNNING;
        }

        (void)sh1_clear();

        return DISPATCH_FAILED;
}

dispatch_state_t
dispatch_poll(dispatch_processor_t processor)
{
        if (processor >= DISPATCH_PROCESSORS) {
                return DISPATCH_FAILED;
        }

        dispatch_job_t *const job = current[processor];

        if (job == NULL) {
                return DISPATCH_IDLE;
        }

        /* Another job's transfer holds the data port: nothing for this one to ask the CD block yet */
        const bool needs_port = (processor == DISPATCH_SH1) || (phase[processor] == PHASE_LOADING);

        if (needs_port && dispatch_port_busy() && (port_owner != processor)) {
                return DISPATCH_RUNNING;
        }

        dispatch_state_t state;

        if (phase[processor] == PHASE_LOADING) {
                state = loading_poll(processor, job);
        } else if (processor == DISPATCH_SLAVE) {
                state = slave_poll(job);
        } else {
                state = sh1_poll(job);
        }

        if (state != DISPATCH_RUNNING) {
                job->state = state;
                current[processor] = NULL;
        }

        return state;
}
