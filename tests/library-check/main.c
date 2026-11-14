/*
 * Checks the card ROM's library with the client's sh1_library_check(), a line on screen for each
 * step: the card loaded and on, AD with the firmware's protocol, then each of the library's five
 * handles checked in turn, with what the check found and the load's job record. Last, the transform
 * module is checked once more and a job run on it, its output compared with the transform computed
 * here, so a module that a check leaves installed runs as any other.
 *
 * What passes depends on the firmware. The demo's card ROM has three modules, on 251 to 253:
 * - Protocol 2, the assembly firmware: 251, 252 and 253 ok, 254 and 255 missing.
 * - Protocol 1, the firmware in C, which has no check: every handle unchecked. The job still runs,
 *   its module loaded by the job itself.
 * A card ROM with a damaged copy fails that handle's line as bad, the reason in the result's high
 * byte: 6 for the CRC.
 */

#include <yaul.h>

#include <string.h>

#include <sh1-client.h>

/* The partitions, each filled through the filter of the same number. */
#define INPUT 0
#define OUTPUT 1

#define VECTORS 252

/* The library's handles, and those the demo's card ROM fills. */
#define HANDLE_FIRST SH1_LIBRARY_FIRST
#define HANDLES 5
#define MODULES 3

static uint8_t put_buffer[SH1_SECTOR_BYTES] __aligned(16);
static uint8_t got[SH1_SECTOR_BYTES] __aligned(16);
static int16_t expected[SH1_SECTOR_BYTES / 2];

/* A rotation by 30 degrees about z in Q12 with a translation, rows of (m0, m1, m2, t). */
static const int16_t matrix[3][4] = {
        { 3547, -2048, 0, 100 },
        { 2048, 3547, 0, -200 },
        { 0, 0, 4096, 300 }
};

static const char *const found_names[] = {
        [SH1_LIBRARY_OK] = "ok",
        [SH1_LIBRARY_MISSING] = "missing",
        [SH1_LIBRARY_BAD] = "bad",
        [SH1_LIBRARY_UNCHECKED] = "unchecked"
};

/* Each job's ID, new for every job so a stale answer cannot pass for its own. */
static uint16_t job_id;

static unsigned line = 4;

/* The line to the screen at the next vertical blank. Interrupts are masked across the flush: a
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
        line++;
}

static void
report(const char *step, bool passed)
{
        dbgio_printf("\x1b[%u;2H%s %-11s %04X %04X %04X %04X\n", line, passed ? "PASS" : "FAIL",
            step, sh1_answer[0], sh1_answer[1], sh1_answer[2], sh1_answer[3]);
        frame_end();
}

static void
report_job(const char *step, bool passed, const sh1_job_t *job)
{
        dbgio_printf("\x1b[%u;2H%s %-15s %02X %04X %08lX\n", line, passed ? "PASS" : "FAIL", step,
            job->state, job->id, (unsigned long)job->result);
        frame_end();
}

/* What a check of handle should find on the demo's card ROM, by the firmware's protocol. */
static sh1_library_t
expected_found(uint8_t handle)
{
        if (sh1_protocol < SH1_PROTOCOL_LIBRARY_CHECK) {
                return SH1_LIBRARY_UNCHECKED;
        }

        return (handle < HANDLE_FIRST + MODULES) ? SH1_LIBRARY_OK : SH1_LIBRARY_MISSING;
}

/* One check, its line the handle, what it found and the load's record. */
static bool
check(uint8_t handle)
{
        char step[16];
        sh1_job_t job = { .state = SH1_JOB_NONE };

        const sh1_library_t found = sh1_library_check(handle, ++job_id, &job);
        const bool passed = (found == expected_found(handle));

        snprintf(step, sizeof(step), "%u %s", handle, found_names[found]);
        report_job(step, passed, &job);

        return passed;
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

        dbgio_puts("\x1b[2;2HLibrary check\n");

        report("E0 E1 load", sh1_card_load());
        report("93 card on", sh1_card_on());

        uint16_t activations = 0;
        report("AD identity", sh1_identity(&activations) && (activations == 1));

        report("48 reset", sh1_reset_selector());

        for (unsigned i = 0; i < HANDLES; i++) {
                (void)check((uint8_t)(HANDLE_FIRST + i));
        }

        /* The transform module once more, so that it is the one a check left in the slot */
        (void)check(SH1_LIBRARY_TRANSFORM);

        make_vertices();
        report("put vectors", sh1_put(INPUT, 1, put_buffer));

        /* A module job's argument: CR3 the input partition and the output filter, CR4 the most
         * output sectors */
        sh1_job_t job = { .state = SH1_JOB_NONE };
        const uint16_t id = ++job_id;
        const uint32_t argument = ((uint32_t)INPUT << 24) | ((uint32_t)OUTPUT << 16) | 1;
        bool passed = sh1_submit(SH1_LIBRARY_TRANSFORM, id, argument) && sh1_wait(id, &job) &&
            (job.state == SH1_JOB_DONE) && (job.result == 1);
        report_job("transform", passed, &job);

        passed = sh1_get(OUTPUT, 0, 1, got) && (memcmp(got, expected, SH1_SECTOR_BYTES) == 0);
        report("get output", passed);

        report("delete", sh1_delete(INPUT, 0, 0xFFFF) && sh1_delete(OUTPUT, 0, 0xFFFF));

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
