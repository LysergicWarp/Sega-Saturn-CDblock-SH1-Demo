/*
 * Dispatch: hand the same job to three of the Saturn's processors, its two SH-2 CPUs and the CD
 * block's SH-1.
 *
 * The Saturn has two SH-2 CPUs, the master (which runs this program) and the slave. Its CD block
 * has a Hitachi SH-1 of its own, which normally only runs the CD drive. With the demo's MPEG card
 * firmware loaded, the SH-1 also takes jobs from the SH-2.
 * (The Saturn has more processors, the sound system's 68EC000 and the SCU's DSP among them, which
 * this demo leaves alone.)
 *
 * The three are reached in very different ways, and this interface hides that:
 *
 * - Master: the job runs right here, inside dispatch_submit(), or inside the dispatch_poll() that
 *   ends its input's copy from the CD block.
 * - Slave: the slave SH-2 shares the master's work RAM. The master leaves the job in memory and
 *   "rings a doorbell" (libyaul's cpu_dual notify), the slave runs the job and marks it done.
 * - SH-1: the SH-1 has its own memory and sees nothing of the SH-2's. Its input is copied into the
 *   CD block's sector buffer, read there from the disc, or moved there from another partition,
 *   the job is started with a CD block command, and its output is copied back once it is done.
 *   The copies go through the CD block's 16-bit data port.
 *
 * So a job is always the same record: where the input is, where the output goes, and a state that
 * says when the output is there. dispatch_submit() starts it, dispatch_poll() is called until the
 * state says done or failed. Each processor runs one job at a time.
 *
 * Copies through the data port run on the SH-2's DMA controller and do not hold the master up:
 * the submit or poll that starts one returns while it runs, and a later poll ends it. The caller
 * does its own work in between, which is where the copy's time goes.
 *
 * The SH-1's operation is a PRS decode (the compression libyaul's bcl tools write). The SH-2s
 * take that, or any function of the caller's: the slave runs it in parallel with the master.
 */

#ifndef DISPATCH_H
#define DISPATCH_H

#include <stdbool.h>
#include <stdint.h>

/* The first CD-block partition the dispatch leaves to the caller (0 is libyaul's, 1 and 2 its). */
#define DISPATCH_FIRST_FREE 3

typedef enum {
        DISPATCH_MASTER,
        DISPATCH_SLAVE,
        DISPATCH_SH1,
        DISPATCH_PROCESSORS
} dispatch_processor_t;

typedef enum {
        /* Not submitted, or taken back after it ended. */
        DISPATCH_IDLE,
        /* Handed to a processor, not finished yet. */
        DISPATCH_RUNNING,
        /* Finished, the output is in place. */
        DISPATCH_DONE,
        /* Refused or failed, the output is not to be trusted. */
        DISPATCH_FAILED
} dispatch_state_t;

/*
 * One PRS decode, or on an SH-2 a call. Fields a job does not use are 0, as in a zeroed record.
 *
 * - input: the PRS stream, 4-byte aligned and padded to whole 2048-byte sectors, since the SH-1's
 *   copy moves whole sectors. 16-byte aligned when input_partition is set, as the copy into it
 *   comes by DMA and the cache is purged over it. input_bytes is the stream's own length.
 * - input_partition: 0, or the CD-block partition (DISPATCH_FIRST_FREE up) whose first sectors
 *   hold the stream, already in the CD block's buffer, as from a read the caller runs. The job
 *   takes those sectors out of the partition:
 *   - On the master or the slave, they cross the data port into input, by DMA.
 *   - On the SH-1, they move inside the CD block into its input partition. Nothing crosses.
 * - input_fad: 0 for a stream already in memory at input. Otherwise the stream is a file on the
 *   disc starting at this frame address, and the job reads it first:
 *   - On the master or the slave, the master reads the file into input, so input must have room
 *     for the file's whole sectors. This is the usual way: the compressed bytes cross from the CD
 *     block to the SH-2, then an SH-2 decodes them.
 *   - On the SH-1, the CD block reads the file into its own buffer and the SH-1 decodes it there.
 *     input is not used: the compressed bytes never cross to the SH-2, only the decoded output.
 * - output: 16-byte aligned (the size of an SH-2 cache line), with room for output_bytes rounded
 *   up to whole sectors, again for the SH-1.
 * - output_bytes: what the stream decodes to. libyaul's decoder has no output limit, so the size
 *   must be known up front. The SH-1 reports the size it decoded, and a job whose size differs
 *   fails.
 * - state: written by dispatch, read by the caller.
 */
typedef struct {
        /* Optional, the SH-2s only: when set, the job is a call of call(work) instead of a decode,
         * and the input and output fields are not used. */
        void (*call)(void *work);
        void *work;
        void *input;
        uint32_t input_bytes;
        uint8_t input_partition;
        uint32_t input_fad;
        void *output;
        uint32_t output_bytes;
        volatile dispatch_state_t state;
} dispatch_job_t;

/*
 * Starts the slave and the SH-1, once, before any job.
 *
 * The slave is started and asked to answer, so a job is never sent before it listens. The SH-1
 * gets the card firmware loaded and turned on, then the decompress module, from package (the
 * module's package, 4-byte aligned and padded to whole sectors, package_bytes long). A processor
 * that does not come up is left out: dispatch_available() says which did.
 *
 * libyaul's cd_block_init(), for its own disc reads, goes before this: its 04 restarts the CD
 * block, which turns the card firmware off.
 */
void dispatch_init(const void *package, uint32_t package_bytes);

/* Whether a processor came up in dispatch_init(). The master always does. */
bool dispatch_available(dispatch_processor_t processor);

/*
 * Hands job to processor. False, with the job failed, when the processor is not available, is
 * still busy with another job, or refuses it, and when the job needs the CD block while a copy
 * holds the data port (dispatch_port_busy()).
 * - On the master a job from memory or the disc has already ended when this returns. One from a
 *   partition is decoded by the poll that ends its copy.
 * - A job from the disc waits here for its read, on every processor.
 */
bool dispatch_submit(dispatch_processor_t processor, dispatch_job_t *job);

/*
 * Moves processor's job along and returns its state: running, then done or failed once, then idle
 * with no job out. The job's own state field follows.
 * - Cheap on the slave, and on the master but for the decode it may run.
 * - On the SH-1 each call asks the CD block, and the call that finds the job done starts the
 *   output's copy back. While another job's copy holds the data port, the SH-1's poll waits.
 */
dispatch_state_t dispatch_poll(dispatch_processor_t processor);

/* Whether a copy holds the data port. Until it ends, nothing else may send the CD block a command,
 * the caller's own reads included. */
bool dispatch_port_busy(void);

#endif /* DISPATCH_H */
