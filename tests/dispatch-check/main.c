/*
 * Runs one PRS decode through the dispatch on each processor in turn, then on all three at once, a
 * line on screen for each: the slave and the SH-1 up, then each run with the frames it took, its
 * output compared with the original. The stream is ../module-check/original.txt compressed by
 * bcl_prs, as the module check's.
 *
 * The parallel run also checks that a busy processor refuses another job and keeps its own, and
 * that each poll gives idle once its job has ended.
 *
 * Last, a cache case, which needs the SH-2s' data caches emulated, as Beetle Saturn does by
 * default. A 256-byte stream is decoded twice on the slave through one job record, the second time
 * into a new output buffer. The decodes are small, so their own memory traffic evicts little:
 * - A slave that skips its purge before a job still has the record's old fields cached and
 *   decodes into the old buffer.
 * - The master reads the new buffer through its cache before the job and polls without drawing a
 *   frame, so a master that skips its purge after the job compares its stale lines.
 *
 * Then the same small stream put into a CD-block partition and decoded from there, on the master
 * (copied by DMA into a buffer the master has cached) and on the SH-1 (moved inside the CD block,
 * its output copied by DMA into a cached buffer). A DMA write goes to memory behind the cache, on
 * hardware and in Beetle Saturn alike, so a copy that skips its purge decodes or compares stale
 * lines and fails here. Each job's output must equal the original, and the partition must be left
 * empty.
 */

#include <yaul.h>

#include <string.h>

#include <dispatch.h>
#include <sh1-client.h>

#define SECTOR_BYTES 2048

#define PACKAGE_SECTORS 1
#define INPUT_SECTORS 8
#define OUTPUT_SECTORS 10

/* Frames a job may take before the check gives up on it. */
#define JOB_FRAMES 600

extern const uint8_t asset_decompress[];
extern const uint8_t asset_decompress_end[];
extern const uint8_t asset_stream[];
extern const uint8_t asset_stream_end[];
extern const uint8_t asset_original[];
extern const uint8_t asset_original_end[];
extern const uint8_t asset_small_stream[];
extern const uint8_t asset_small_stream_end[];
extern const uint8_t asset_small_original[];
extern const uint8_t asset_small_original_end[];

static const char *const names[DISPATCH_PROCESSORS] = {
        "Master decode selftest", "Slave decode selftest", "SH-1 decode selftest"
};

static uint8_t package[PACKAGE_SECTORS * SECTOR_BYTES] __aligned(16);
static uint8_t input[INPUT_SECTORS * SECTOR_BYTES] __aligned(16);
static uint8_t output[DISPATCH_PROCESSORS][OUTPUT_SECTORS * SECTOR_BYTES] __aligned(16);
static uint8_t spare_output[OUTPUT_SECTORS * SECTOR_BYTES] __aligned(16);
static uint8_t small_input[SECTOR_BYTES] __aligned(16);
static uint8_t small_output[2][SECTOR_BYTES] __aligned(16);
static uint8_t copy_input[SECTOR_BYTES] __aligned(16);

static dispatch_job_t jobs[DISPATCH_PROCESSORS];
static dispatch_job_t spare;
static dispatch_job_t cache_job;
static dispatch_job_t partition_job;

/* Where the warming reads go, so the compiler keeps them. */
static volatile uint32_t warmed;

/* Polls of the slave in the cache case before it gives up, some seconds. */
#define CACHE_POLLS 0x1000000UL

static unsigned line = 4;

/* The text to the screen at the next vertical blank. Interrupts are masked across the flush: a
 * vertical blank inside it would leave it unmarked, and libyaul drops every flush after that. */
static void
frame_end(void)
{
        const uint32_t mask = cpu_intc_mask_get();

        cpu_intc_mask_set(15);
        dbgio_flush();
        cpu_intc_mask_set(mask);

        vdp2_sync();
        vdp2_sync_wait();
}

static void
report(const char *step, bool passed, const char *detail)
{
        dbgio_printf("\x1b[%u;2H%s %-24s %s\n", line, passed ? "PASS" : "FAIL", step, detail);
        frame_end();
        line++;
}

/* Copies the bytes from start to end into buffer, zeros after them to whole sectors. Returns the
 * bytes, or 0 for more than the sectors given. */
static uint32_t
stage(uint8_t *buffer, uint32_t sectors, const uint8_t *start, const uint8_t *end)
{
        const uint32_t bytes = (uint32_t)(end - start);
        const uint32_t padded = (bytes + SECTOR_BYTES - 1) / SECTOR_BYTES * SECTOR_BYTES;

        if (padded > sectors * SECTOR_BYTES) {
                return 0;
        }

        memcpy(buffer, start, bytes);
        memset(buffer + bytes, 0, padded - bytes);

        return bytes;
}

/* Fills the job's output with A5, so a job that writes nothing fails, then reads it a byte a cache
 * line, so the master's cache holds its last lines when the job starts. */
static void
prepare(dispatch_job_t *job, uint8_t *buffer, uint32_t input_bytes)
{
        uint32_t sum = 0;

        memset(buffer, 0xA5, OUTPUT_SECTORS * SECTOR_BYTES);

        for (uint32_t i = 0; i < OUTPUT_SECTORS * SECTOR_BYTES; i += 16) {
                sum += buffer[i];
        }

        warmed = sum;

        job->input = input;
        job->input_bytes = input_bytes;
        job->output = buffer;
        job->output_bytes = (uint32_t)(asset_original_end - asset_original);
        job->state = DISPATCH_IDLE;
}

static bool
matches(const dispatch_job_t *job)
{
        return (job->state == DISPATCH_DONE) &&
            (memcmp(job->output, asset_original, job->output_bytes) == 0);
}

/* Polls the processors in mask once a frame until none is running. Returns the frames. */
static uint32_t
poll_all(uint32_t mask)
{
        uint32_t frames = 0;

        while (frames < JOB_FRAMES) {
                bool running = false;

                for (unsigned p = 0; p < DISPATCH_PROCESSORS; p++) {
                        if (((mask & (1U << p)) != 0) && (dispatch_poll(p) == DISPATCH_RUNNING)) {
                                running = true;
                        }
                }

                if (!running) {
                        break;
                }

                frame_end();
                frames++;
        }

        return frames;
}

/* One small decode on the slave into buffer through cache_job, polled without a frame. Before the
 * job the buffer is filled with A5 and read a byte a cache line, the whole sector, so the master's
 * cache holds every line of it. */
static bool
small_decode(uint8_t *buffer)
{
        uint32_t sum = 0;

        memset(buffer, 0xA5, SECTOR_BYTES);

        for (uint32_t i = 0; i < SECTOR_BYTES; i += 16) {
                sum += buffer[i];
        }

        warmed = sum;

        cache_job.output = buffer;

        if (!dispatch_submit(DISPATCH_SLAVE, &cache_job)) {
                return false;
        }

        for (uint32_t i = 0; i < CACHE_POLLS; i++) {
                if (dispatch_poll(DISPATCH_SLAVE) != DISPATCH_RUNNING) {
                        break;
                }
        }

        return matches(&cache_job);
}

/* The cache case: the same record twice, the second time with a new output buffer. The small
 * original is the original's first 256 bytes, so matches() compares it as it is. */
static bool
cache_case(void)
{
        const uint32_t bytes = (uint32_t)(asset_small_stream_end - asset_small_stream);

        memset(small_input, 0, sizeof(small_input));
        memcpy(small_input, asset_small_stream, bytes);

        cache_job.input = small_input;
        cache_job.input_bytes = bytes;
        cache_job.output_bytes = (uint32_t)(asset_small_original_end - asset_small_original);

        return small_decode(small_output[0]) && small_decode(small_output[1]);
}

/* buffer filled with A5 and read a byte a cache line, so the master's cache holds all of it. */
static void
warm(uint8_t *buffer)
{
        uint32_t sum = 0;

        memset(buffer, 0xA5, SECTOR_BYTES);

        for (uint32_t i = 0; i < SECTOR_BYTES; i += 16) {
                sum += buffer[i];
        }

        warmed = sum;
}

/* The small stream put into partition DISPATCH_FIRST_FREE, then decoded by processor from there,
 * polled without a frame. */
static bool
partition_case(dispatch_processor_t processor)
{
        const uint32_t bytes = (uint32_t)(asset_small_stream_end - asset_small_stream);

        memset(small_input, 0, sizeof(small_input));
        memcpy(small_input, asset_small_stream, bytes);

        if (!sh1_put(DISPATCH_FIRST_FREE, 1, small_input)) {
                return false;
        }

        warm(copy_input);
        warm(small_output[0]);

        partition_job.input = copy_input;
        partition_job.input_bytes = bytes;
        partition_job.input_partition = DISPATCH_FIRST_FREE;
        partition_job.output = small_output[0];
        partition_job.output_bytes = (uint32_t)(asset_small_original_end - asset_small_original);
        partition_job.state = DISPATCH_IDLE;

        bool passed = dispatch_submit(processor, &partition_job);

        for (uint32_t i = 0; passed && (i < CACHE_POLLS); i++) {
                if (dispatch_poll(processor) != DISPATCH_RUNNING) {
                        break;
                }
        }

        passed = passed && matches(&partition_job);

        /* The job took the sector, so the partition is empty again: 51 says so */
        uint16_t left = 0xFFFF;

        return passed && sh1_sectors(DISPATCH_FIRST_FREE, &left) && (left == 0);
}

int
main(void)
{
        dbgio_init();
        dbgio_dev_default_init(DBGIO_DEV_VDP2_ASYNC);
        dbgio_dev_font_load();

        dbgio_puts("\x1b[2;2HCompute selftest\n");
        frame_end();

        const uint32_t package_bytes =
            stage(package, PACKAGE_SECTORS, asset_decompress, asset_decompress_end);
        const uint32_t input_bytes = stage(input, INPUT_SECTORS, asset_stream, asset_stream_end);

        dispatch_init(package, package_bytes);

        report("Slave SH-2 awake", dispatch_available(DISPATCH_SLAVE), "");
        report("CD-block SH-1 awake", dispatch_available(DISPATCH_SH1), "");

        char detail[24];

        for (unsigned p = 0; p < DISPATCH_PROCESSORS; p++) {
                prepare(&jobs[p], output[p], input_bytes);

                const bool submitted = dispatch_submit(p, &jobs[p]);
                const uint32_t frames = poll_all(1U << p);

                snprintf(detail, sizeof(detail), "%lu frames", (unsigned long)frames);
                report(names[p], submitted && matches(&jobs[p]), detail);
        }

        /* All three at once: the slave and the SH-1 first, so they run while the master decodes */
        prepare(&jobs[DISPATCH_MASTER], output[DISPATCH_MASTER], input_bytes);
        prepare(&jobs[DISPATCH_SLAVE], output[DISPATCH_SLAVE], input_bytes);
        prepare(&jobs[DISPATCH_SH1], output[DISPATCH_SH1], input_bytes);
        prepare(&spare, spare_output, input_bytes);

        bool passed = dispatch_submit(DISPATCH_SLAVE, &jobs[DISPATCH_SLAVE]) &&
            dispatch_submit(DISPATCH_SH1, &jobs[DISPATCH_SH1]);

        /* Busy: another job is refused and failed, the running job given again keeps its state */
        passed = passed && !dispatch_submit(DISPATCH_SLAVE, &spare) &&
            (spare.state == DISPATCH_FAILED) &&
            !dispatch_submit(DISPATCH_SH1, &jobs[DISPATCH_SH1]) &&
            (jobs[DISPATCH_SH1].state == DISPATCH_RUNNING);

        passed = dispatch_submit(DISPATCH_MASTER, &jobs[DISPATCH_MASTER]) && passed;

        const uint32_t frames = poll_all((1U << DISPATCH_SLAVE) | (1U << DISPATCH_SH1));

        passed = passed && (dispatch_poll(DISPATCH_SLAVE) == DISPATCH_IDLE) &&
            (dispatch_poll(DISPATCH_SH1) == DISPATCH_IDLE) && matches(&jobs[DISPATCH_MASTER]) &&
            matches(&jobs[DISPATCH_SLAVE]) && matches(&jobs[DISPATCH_SH1]);

        snprintf(detail, sizeof(detail), "%lu frames", (unsigned long)frames);
        report("Parallel decode selftest", passed, detail);

        report("Cache coherency selftest", dispatch_available(DISPATCH_SLAVE) && cache_case(), "");
        report("CD buffer copy selftest", partition_case(DISPATCH_MASTER), "");
        report("CD buffer move selftest",
            dispatch_available(DISPATCH_SH1) && partition_case(DISPATCH_SH1), "");

        while (true) {
        }

        return 0;
}

void
user_init(void)
{
        vdp2_tvmd_display_res_set(VDP2_TVMD_INTERLACE_NONE, VDP2_TVMD_HORZ_NORMAL_A,
            VDP2_TVMD_VERT_224);

        vdp2_scrn_back_color_set(VDP2_VRAM_ADDR(3, 0x01FFFE),
            RGB1555(1, 0, 0, 7));

        vdp2_tvmd_display_set();
}
