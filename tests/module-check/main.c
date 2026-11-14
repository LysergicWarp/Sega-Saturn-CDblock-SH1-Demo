/*
 * Runs the card firmware's two modules, a line on screen for each step: the card loaded and on, AD
 * with the firmware's protocol, the decompress module loaded, a PRS stream of original.txt put and
 * decoded into sectors that are got back and compared with the original, then the transform module
 * loaded in its place, the decompress module's handle rejected and a sector of vertices transformed
 * and compared with the transform computed here.
 */

#include <yaul.h>

#include <string.h>

#include <sh1-client.h>

/* The partitions, each filled through the filter of the same number. */
#define INPUT 0
#define OUTPUT 1

#define JOB_ID 1

/* The buffer's slots. Those the input leaves free bound the output. */
#define SLOTS 200

#define PUT_SECTORS 8
#define GOT_SECTORS 10
#define VECTORS 252

extern const uint8_t asset_decompress[];
extern const uint8_t asset_decompress_end[];
extern const uint8_t asset_transform[];
extern const uint8_t asset_transform_end[];
extern const uint8_t asset_stream[];
extern const uint8_t asset_stream_end[];
extern const uint8_t asset_original[];
extern const uint8_t asset_original_end[];

static uint8_t put_buffer[PUT_SECTORS * SH1_SECTOR_BYTES] __aligned(16);
static uint8_t got[GOT_SECTORS * SH1_SECTOR_BYTES] __aligned(16);
static int16_t expected[SH1_SECTOR_BYTES / 2];

/* A rotation by 30 degrees about z in Q12 with a translation, rows of (m0, m1, m2, t). */
static const int16_t matrix[3][4] = {
        { 3547, -2048, 0, 100 },
        { 2048, 3547, 0, -200 },
        { 0, 0, 4096, 300 }
};

static unsigned line = 4;

/* The text into the queue for the next vertical blank. Interrupts are masked across the flush: a
 * vertical blank inside it would leave it unmarked, and libyaul drops every flush after that. */
static void
text_flush(void)
{
        const uint32_t mask = cpu_intc_mask_get();

        cpu_intc_mask_set(15);
        dbgio_flush();
        cpu_intc_mask_set(mask);
}

static void
report(const char *step, bool passed)
{
        dbgio_printf("\x1b[%u;2H%s %-11s %04X %04X %04X %04X\n", line, passed ? "PASS" : "FAIL", step,
            sh1_answer[0], sh1_answer[1], sh1_answer[2], sh1_answer[3]);
        text_flush();
        vdp2_sync();
        vdp2_sync_wait();
        line++;
}

static void
report_job(const char *step, bool passed, const sh1_job_t *job)
{
        dbgio_printf("\x1b[%u;2H%s %-15s %02X %04X %08lX\n", line, passed ? "PASS" : "FAIL", step,
            job->state, job->id, (unsigned long)job->result);
        text_flush();
        vdp2_sync();
        vdp2_sync_wait();
        line++;
}

/* Copies the bytes from start to end into the put buffer, zeros after them to whole sectors.
 * Returns the sectors, or 0 for more than the buffer holds. */
static uint16_t
stage(const uint8_t *start, const uint8_t *end)
{
        const uint32_t bytes = (uint32_t)(end - start);
        const uint32_t sectors = (bytes + SH1_SECTOR_BYTES - 1) / SH1_SECTOR_BYTES;

        if (sectors > PUT_SECTORS) {
                return 0;
        }

        memcpy(put_buffer, start, bytes);
        memset(put_buffer + bytes, 0, sectors * SH1_SECTOR_BYTES - bytes);

        return (uint16_t)sectors;
}

static bool
load(const uint8_t *start, const uint8_t *end, sh1_job_t *job)
{
        const uint16_t sectors = stage(start, end);

        job->state = SH1_JOB_NONE;

        return (sectors != 0) && sh1_module_load(INPUT, put_buffer, sectors, JOB_ID, job);
}

/* A module job's argument: CR3 the input partition and the output filter, CR4 the most output
 * sectors. */
static uint32_t
argument(uint16_t limit)
{
        return ((uint32_t)INPUT << 24) | ((uint32_t)OUTPUT << 16) | limit;
}

static bool
run_job(uint8_t handle, uint16_t limit, sh1_job_t *job)
{
        job->state = SH1_JOB_NONE;

        return sh1_submit(handle, JOB_ID, argument(limit)) && sh1_wait(JOB_ID, job) &&
            (job->state == SH1_JOB_DONE);
}

static bool
delete_both(void)
{
        return sh1_delete(INPUT, 0, 0xFFFF) && sh1_delete(OUTPUT, 0, 0xFFFF);
}

/* The sector of vertices into the put buffer and its transform into expected: the matrix padded to
 * 32 bytes, then vectors of (x, y, z, w). Each row of the transform gives the low 16 bits of
 * ((m0 x + m1 y + m2 z) >> 12) + t from a 32-bit sum, and w is copied. */
static void
make_vertices(void)
{
        int16_t *const in = (int16_t *)put_buffer;

        for (unsigned i = 0; i < 16; i++) {
                in[i] = (i < 12) ? matrix[i / 4][i % 4] : 0;
                expected[i] = in[i];
        }

        for (int i = 0; i < VECTORS; i++) {
                int16_t *const x = &in[16 + 4 * i];
                int16_t *const out = &expected[16 + 4 * i];

                x[0] = (int16_t)((i * 37) % 4096 - 2048);
                x[1] = (int16_t)((i * 91) % 4096 - 2048);
                x[2] = (int16_t)((i * 13) % 2048 - 1024);
                x[3] = (int16_t)i;

                for (unsigned r = 0; r < 3; r++) {
                        const int16_t *const m = matrix[r];
                        const uint32_t sum = (uint32_t)(m[0] * x[0] + m[1] * x[1] + m[2] * x[2]);

                        out[r] = (int16_t)(uint16_t)((sum >> 12) + (uint32_t)m[3]);
                }

                out[3] = x[3];
        }
}

int
main(void)
{
        dbgio_init();
        dbgio_dev_default_init(DBGIO_DEV_VDP2_ASYNC);
        dbgio_dev_font_load();

        dbgio_puts("\x1b[2;2HModule check\n");

        report("E0 E1 load", sh1_card_load());
        report("93 card on", sh1_card_on());

        uint16_t activations = 0;
        bool passed = sh1_identity(&activations) && (activations == 1);
        report("AD identity", passed);

        report("48 reset", sh1_reset_selector());

        sh1_job_t job;
        passed = load(asset_decompress, asset_decompress_end, &job);
        report_job("load decompress", passed, &job);
        const uint8_t decompress = (uint8_t)job.result;

        const uint32_t original = (uint32_t)(asset_original_end - asset_original);
        const uint16_t sectors = stage(asset_stream, asset_stream_end);
        passed = (sectors != 0) && sh1_put(INPUT, sectors, put_buffer);
        report("put stream", passed);

        passed = run_job(decompress, SLOTS - sectors, &job) && (job.result == original);
        report_job("decompress", passed, &job);

        const uint32_t filed = (job.result + SH1_SECTOR_BYTES - 1) / SH1_SECTOR_BYTES;
        passed = (filed <= GOT_SECTORS) && sh1_get(OUTPUT, 0, (uint16_t)filed, got) &&
            (memcmp(got, asset_original, original) == 0);
        report("get output", passed);

        report("delete", delete_both());

        passed = load(asset_transform, asset_transform_end, &job);
        report_job("load transform", passed, &job);
        const uint8_t transform = (uint8_t)job.result;

        passed = !sh1_submit(decompress, JOB_ID, argument(1)) &&
            ((sh1_answer[0] >> 8) == SH1_STATUS_REJECT);
        report("old handle", passed);

        make_vertices();
        report("put vectors", sh1_put(INPUT, 1, put_buffer));

        passed = run_job(transform, 1, &job) && (job.result == 1);
        report_job("transform", passed, &job);

        passed = sh1_get(OUTPUT, 0, 1, got) && (memcmp(got, expected, SH1_SECTOR_BYTES) == 0);
        report("get output", passed);

        report("delete", delete_both());

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
