/*
 * SH-1 instruction timing, with the card firmware off the disc, for a console with no MPEG card:
 * how many cycles the CD block's SH-1 takes over each kind of instruction when it runs a module's
 * code from the CD buffer's RAM, so that an emulator's timing can be set from it.
 *
 * The timing module (src/modules-asm/timing-module.s) times 16 kernels of 256 instructions each on
 * the SH-1's own clock, ITU4 at a count per 8 cycles. Each kernel runs SAMPLES times, and a job
 * gives the shortest and the clean samples' sum, those that no interrupt lengthened. The master
 * also times each job by its free-running timer.
 *
 * Three conditions, the same kernels in each:
 * - idle: the drive left as the disc check leaves it.
 * - read: the drive plays the disc into partition 2, and once a frame the master deletes what
 *   arrived (51 and 62). Each sector reaches the RAM by the SH-1's DMA channel 0.
 * - xfer: the same, but the master takes the sectors through the data port (63), by the SH-1's
 *   DMA channel 1.
 *
 * Each kernel row gives two figures a condition, in cycles an instruction:
 * - best: the clean samples' mean less the empty kernel's, over 256.
 * - wall: the job's time on the master's clock less the empty kernel's, over the samples and 256,
 *   at the SH-1's 20 MHz. What a job gets of the SH-1, interrupts and the CD tasks included.
 * The empty kernel's row gives the frame both are measured from, in cycles a sample.
 *
 * Below the table: the drive state (the status byte's low nibble) at each condition's start and
 * end, and the sectors a second that arrived while each streaming condition ran.
 *
 * The SH-1 comes up as in tests/disc-particles: this disc's header names
 * src/disc-extensions/firmware-boot-asm as its disc extension, E0 runs it, and line 3 shows E1's
 * result and 01's CD block revision and hardware flag. Then 93, AD, and the timing module uploaded.
 */

#include <yaul.h>

#include <string.h>

#include <sh1-client.h>

#define KERNELS 16
#define SAMPLES 1024
#define KERNEL_INSTRUCTIONS 256

/* The module's argument: the operation in bits 31..24, the samples in 23..8, the kernel in 7..0. */
#define OP_SAMPLE 0x00000000UL
#define OP_CLEAN_SUM 0x01000000UL

/* The CD block's interrupt flags register, for E0's end, EFLS, and a job's end, MPED. */
#define CD_HIRQ (*(volatile uint16_t *)0x25890008UL)
#define HIRQ_EFLS 0x0200

/* Frames E0 may take, past the firmware's own 20 s limit on the disc check. */
#define AUTH_FRAMES 1500

/* The CD block revision the disc extension sets, its byte +0x44. */
#define EXTENSION_REVISION 0xB0

/* The partition the module's package is put through, and the most sectors a package takes. */
#define PACKAGE_PARTITION 1
#define PACKAGE_SECTORS_MOST 8

/* The streaming conditions' filter and partition, and the disc they play: from the first data
 * sector, over the disc's ZFILL.BIN, about 11 s of play. */
#define STREAM_FILTER 2
#define STREAM_PARTITION 2
#define STREAM_FAD 150
#define STREAM_SECTORS 1600

/* The most sectors one 63 takes. */
#define XFER_MOST 8

/* Frames a job, and the first sector of a streaming condition, may take. */
#define JOB_FRAMES 600
#define START_FRAMES 600

/* The SH-1's clock, and the master's in a 320 pixel wide mode. */
#define SH1_HZ 20000000ULL
#define NTSC_HZ 26846591ULL
#define PAL_HZ 26687500ULL

/* The free-running timer counts the master's clock / 32. */
#define FRT_DIVIDER 32ULL

/* The screen's rows. */
#define ROW_HEADER 4
#define ROW_KERNELS 6
#define ROW_DRIVE 23
#define ROW_RATE 24
#define ROW_STATUS 26

typedef enum {
        IDLE,
        READ,
        XFER,
        CONDITIONS
} condition_t;

static const char *const condition_names[CONDITIONS] = { "idle", "read", "xfer" };

static const char *const kernel_names[KERNELS] = {
        "empty", "alu", "bra", "bt", "muls", "rdw", "rdl", "rdw far",
        "rdl far", "wrw", "wrl", "ld-use", "rd ram", "wr ram", "rd reg", "rd if"
};

/* A kernel's measure in one condition. */
typedef struct {
        bool ok;
        uint32_t shortest;      /* counts */
        uint32_t clean;         /* samples */
        uint32_t clean_sum;     /* counts */
        uint32_t ticks;         /* the job's time on the master's clock */
} measure_t;

static measure_t measures[CONDITIONS][KERNELS];

/* Each condition's drive state at its start and end, and the sectors that arrived over its time. */
static uint8_t drive_before[CONDITIONS];
static uint8_t drive_after[CONDITIONS];
static uint32_t condition_sectors[CONDITIONS];
static uint32_t condition_ticks[CONDITIONS];

/* The timing module's package. */
extern const uint8_t asset_timing[];
extern const uint8_t asset_timing_end[];

/* The package is put into the CD block in whole sectors, so its copy is padded to them. */
static uint8_t package[PACKAGE_SECTORS_MOST * SH1_SECTOR_BYTES] __aligned(16);

/* Where xfer's sectors land. */
static uint8_t xfer_buffer[XFER_MOST * SH1_SECTOR_BYTES] __aligned(16);

/* Video frames since the start, counted at each vertical blank's start. */
static volatile uint32_t frames;

static uint64_t master_hz = NTSC_HZ;

/* The module's handle, and the ID of the SH-1's last job. */
static uint8_t timing_handle;
static uint16_t job_id;

/* The condition running, and the sectors it has moved since its count was cleared. */
static condition_t stream = IDLE;
static uint32_t streamed;

/* The timer counts 16 bits, about 78 ms at the clock / 32. clock_now() widens it to 32 bits as long
 * as it is called at least that often. */
static uint16_t clock_last;
static uint32_t clock_elapsed;

static void
vblank_in(void *work __unused)
{
        frames++;
}

/* Ends a frame: the text written so far goes to VDP2 at the next vertical blank. Interrupts are
 * masked across the flush: a vertical blank inside it would leave it unmarked, and libyaul drops
 * every flush after that. */
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
clock_start(void)
{
        cpu_frt_init(CPU_FRT_CLOCK_DIV_32);
        clock_last = cpu_frt_count_get();
        clock_elapsed = 0;
}

static uint32_t
clock_now(void)
{
        const uint16_t count = cpu_frt_count_get();

        clock_elapsed += (uint16_t)(count - clock_last);
        clock_last = count;

        return clock_elapsed;
}

/* Waits for the next vertical blank, the clock kept up meanwhile. */
static void
frame_wait(void)
{
        const uint32_t now = frames;

        while (frames == now) {
                (void)clock_now();
        }
}

/* The drive state, the low nibble of 00's status byte, or 0xF when 00 fails. */
static uint8_t
drive_state(void)
{
        if (!sh1_command(0x0000, 0x0000, 0x0000, 0x0000, 0)) {
                return 0x0F;
        }

        return (uint8_t)((sh1_answer[0] >> 8) & 0x0F);
}

/* E0 subcode 0 on partition 0, as the BIOS sends it, then its end, EFLS, a frame at a time. */
static bool
authenticate(void)
{
        CD_HIRQ = (uint16_t)~HIRQ_EFLS;

        if (!sh1_command(0xE000, 0x0000, 0x0000, 0x0000, 0) ||
            ((sh1_answer[0] >> 8) == SH1_STATUS_REJECT)) {
                return false;
        }

        for (uint32_t i = 0; i < AUTH_FRAMES; i++) {
                if ((CD_HIRQ & HIRQ_EFLS) != 0) {
                        return true;
                }

                frame_end();
        }

        return false;
}

/*
 * The SH-1 up from the disc: E0, then E1's result and 01's revision and hardware flag on line 3,
 * then 93, AD, the selector reset and the timing module's load. True once the module is installed,
 * its handle in timing_handle.
 */
static bool
sh1_start(void)
{
        uint8_t result = 0xFF;
        uint8_t revision = 0xFF;
        uint8_t flag = 0xFF;
        const char *failed = NULL;

        dbgio_puts("\x1b[3;2HE0, disc check\n");
        frame_end();

        if (!authenticate()) {
                failed = "E0";
        }

        if (sh1_command(0xE100, 0x0000, 0x0000, 0x0000, 0) &&
            ((sh1_answer[0] >> 8) != SH1_STATUS_REJECT)) {
                result = sh1_answer[1] & 0xFF;
        }

        if (sh1_command(0x0100, 0x0000, 0x0000, 0x0000, 0) &&
            ((sh1_answer[0] >> 8) != SH1_STATUS_REJECT)) {
                flag = sh1_answer[1] >> 8;
                revision = sh1_answer[3] & 0xFF;
        }

        if ((failed == NULL) && (revision != EXTENSION_REVISION)) {
                failed = "no ext";
        } else if ((failed == NULL) && ((flag & 0x02) == 0)) {
                failed = "no card bit";
        }

        uint16_t activations;
        sh1_job_t job;

        if ((failed == NULL) && !sh1_card_on()) {
                failed = "93";
        } else if ((failed == NULL) && !sh1_identity(&activations)) {
                failed = "AD";
        } else if ((failed == NULL) && !sh1_reset_selector()) {
                failed = "48";
        }

        if (failed == NULL) {
                const uint32_t bytes = (uint32_t)(asset_timing_end - asset_timing);
                const uint16_t sectors =
                    (uint16_t)((bytes + SH1_SECTOR_BYTES - 1) / SH1_SECTOR_BYTES);

                if ((bytes == 0) || (bytes > sizeof(package))) {
                        failed = "package";
                } else {
                        memcpy(package, asset_timing, bytes);

                        if (!sh1_module_load(PACKAGE_PARTITION, package, sectors, ++job_id,
                                &job) || (job.state != SH1_JOB_DONE)) {
                                failed = "load";
                        } else {
                                timing_handle = (uint8_t)job.result;
                        }
                }
        }

        dbgio_printf("\x1b[3;2HE1 %02X rev %02X hw %02X %-12s\n", result, revision, flag,
            (failed == NULL) ? "SH-1 up" : failed);
        frame_end();

        return (failed == NULL);
}

/*
 * Once a frame while a streaming condition runs: the sectors that arrived in its partition, deleted
 * (read) or taken through the data port (xfer), and counted.
 */
static void
stream_service(void)
{
        if (stream == IDLE) {
                return;
        }

        uint16_t count;

        if (!sh1_sectors(STREAM_PARTITION, &count) || (count == 0)) {
                return;
        }

        if (stream == READ) {
                if (sh1_delete(STREAM_PARTITION, 0, count)) {
                        streamed += count;
                }

                return;
        }

        if (count > XFER_MOST) {
                count = XFER_MOST;
        }

        if (sh1_get_start(STREAM_PARTITION, 0, count, xfer_buffer, true) &&
            sh1_transfer_end()) {
                streamed += count;
        }
}

/* A condition's start: for read and xfer, the drive connected and playing, true once its first
 * sectors have arrived. */
static bool
stream_start(condition_t condition)
{
        stream = condition;
        streamed = 0;

        if (condition == IDLE) {
                return true;
        }

        (void)sh1_delete(STREAM_PARTITION, 0, 0xFFFF);

        if (!sh1_connect(STREAM_FILTER) || !sh1_play(STREAM_FAD, STREAM_SECTORS)) {
                return false;
        }

        for (uint32_t i = 0; i < START_FRAMES; i++) {
                frame_wait();
                stream_service();

                if (streamed != 0) {
                        return true;
                }
        }

        return false;
}

/* A condition's end: no more sectors, and the partition emptied. */
static void
stream_stop(void)
{
        if (stream != IDLE) {
                sh1_disconnect();
                (void)sh1_delete(STREAM_PARTITION, 0, 0xFFFF);
        }

        stream = IDLE;
}

/*
 * One job to its end, the condition's sectors serviced once a frame meanwhile. True when it ends
 * done, its result in *result and its time from AB to MPED in *ticks.
 */
static bool
job_run(uint32_t argument, uint32_t *result, uint32_t *ticks)
{
        sh1_job_t job;

        CD_HIRQ = (uint16_t)~SH1_MPED;

        const uint32_t start = clock_now();

        if (!sh1_submit(timing_handle, ++job_id, argument)) {
                return false;
        }

        const uint32_t limit = frames + JOB_FRAMES;
        uint32_t seen = frames;

        while ((int32_t)(frames - limit) < 0) {
                const uint32_t now = clock_now();

                /* MPED cleared before AC, so a job still out after it sets it again */
                if ((CD_HIRQ & SH1_MPED) != 0) {
                        CD_HIRQ = (uint16_t)~SH1_MPED;

                        if (!sh1_status(&job) || (job.id != job_id)) {
                                return false;
                        }

                        if (job.state == SH1_JOB_DONE) {
                                *result = job.result;
                                *ticks = now - start;
                                return true;
                        }

                        if (job.state == SH1_JOB_FAILED) {
                                return false;
                        }

                        continue;
                }

                if (frames != seen) {
                        seen = frames;
                        stream_service();
                }
        }

        return false;
}

/* Every kernel in one condition. */
static void
condition_run(condition_t condition)
{
        dbgio_printf("\x1b[%u;2H%-38s\n", ROW_STATUS, "");
        dbgio_printf("\x1b[%u;2H%s running\n", ROW_STATUS, condition_names[condition]);
        frame_end();

        drive_before[condition] = drive_state();

        if (!stream_start(condition)) {
                dbgio_printf("\x1b[%u;2H%s: no sectors arrived\n", ROW_STATUS,
                    condition_names[condition]);
                frame_end();
                stream_stop();
                return;
        }

        streamed = 0;

        const uint32_t start = clock_now();

        for (uint32_t k = 0; k < KERNELS; k++) {
                measure_t *const m = &measures[condition][k];
                uint32_t result;
                uint32_t sum;
                uint32_t ticks;
                uint32_t unused;

                m->ok = job_run(OP_SAMPLE | ((uint32_t)SAMPLES << 8) | k, &result, &ticks) &&
                    job_run(OP_CLEAN_SUM, &sum, &unused);

                if (m->ok) {
                        m->shortest = result >> 16;
                        m->clean = result & 0xFFFF;
                        m->clean_sum = sum;
                        m->ticks = ticks;
                        m->ok = (m->clean != 0);
                }
        }

        condition_ticks[condition] = clock_now() - start;
        condition_sectors[condition] = streamed;
        drive_after[condition] = drive_state();

        stream_stop();
}

/* A clean mean in thousandths of a count. */
static uint64_t
clean_mean(const measure_t *m)
{
        return ((uint64_t)m->clean_sum * 1000) / m->clean;
}

/* Hundredths, in four columns: 9.99, 99.9 or 9999. */
static void
value_print(unsigned row, unsigned column, uint64_t hundredths)
{
        const unsigned long whole = (unsigned long)(hundredths / 100);
        const unsigned long part = (unsigned long)(hundredths % 100);

        if (hundredths < 1000) {
                dbgio_printf("\x1b[%u;%uH%lu.%02lu\n", row, column, whole, part);
        } else if (hundredths < 10000) {
                dbgio_printf("\x1b[%u;%uH%2lu.%lu\n", row, column, whole, part / 10);
        } else {
                dbgio_printf("\x1b[%u;%uH%4lu\n", row, column, whole);
        }
}

static void
missing_print(unsigned row, unsigned column)
{
        dbgio_printf("\x1b[%u;%uH  --\n", row, column);
}

/* Master clock ticks as SH-1 cycles, times scale. */
static uint64_t
ticks_cycles(uint64_t ticks, uint64_t scale)
{
        return (ticks * FRT_DIVIDER * SH1_HZ * scale) / master_hz;
}

static void
table_print(void)
{
        for (uint32_t c = 0; c < CONDITIONS; c++) {
                const unsigned best_column = 10 + (10 * c);
                const unsigned wall_column = best_column + 5;
                const measure_t *const empty = &measures[c][0];

                for (uint32_t k = 0; k < KERNELS; k++) {
                        const unsigned row = ROW_KERNELS + k;
                        const measure_t *const m = &measures[c][k];

                        if (!m->ok || !empty->ok) {
                                missing_print(row, best_column);
                                missing_print(row, wall_column);
                                continue;
                        }

                        if (k == 0) {
                                /* The frame, in cycles a sample */
                                value_print(row, best_column, (clean_mean(m) * 8) / 10);
                                value_print(row, wall_column,
                                    ticks_cycles(m->ticks, 100) / SAMPLES);
                                continue;
                        }

                        const uint64_t mean = clean_mean(m);
                        const uint64_t frame = clean_mean(empty);

                        if (mean < frame) {
                                missing_print(row, best_column);
                        } else {
                                /* Thousandths of a count, 8 cycles each, over 256: / 320 */
                                value_print(row, best_column, (mean - frame + 160) / 320);
                        }

                        if (m->ticks < empty->ticks) {
                                missing_print(row, wall_column);
                        } else {
                                value_print(row, wall_column,
                                    ticks_cycles(m->ticks - empty->ticks, 100) /
                                    ((uint64_t)SAMPLES * KERNEL_INSTRUCTIONS));
                        }
                }
        }

        dbgio_printf("\x1b[%u;2Hdrive idle %X-%X read %X-%X xfer %X-%X\n", ROW_DRIVE,
            drive_before[IDLE], drive_after[IDLE], drive_before[READ], drive_after[READ],
            drive_before[XFER], drive_after[XFER]);

        dbgio_printf("\x1b[%u;2Hsectors/s read\n", ROW_RATE);
        dbgio_printf("\x1b[%u;24Hxfer\n", ROW_RATE);

        for (uint32_t c = READ; c < CONDITIONS; c++) {
                const unsigned column = (c == READ) ? 17 : 29;

                if (condition_ticks[c] == 0) {
                        missing_print(ROW_RATE, column);
                        continue;
                }

                /* Sectors over seconds, the seconds ticks * 32 / the master's clock */
                value_print(ROW_RATE, column,
                    ((uint64_t)condition_sectors[c] * master_hz * 100) /
                    ((uint64_t)condition_ticks[c] * FRT_DIVIDER));
        }
}

int
main(void)
{
        dbgio_init();
        dbgio_dev_default_init(DBGIO_DEV_VDP2_ASYNC);
        dbgio_dev_font_load();

        if (vdp2_tvmd_tv_standard_get() == VDP2_TVMD_TV_STANDARD_PAL) {
                master_hz = PAL_HZ;
        }

        vdp_sync_vblank_in_set(vblank_in, NULL);

        dbgio_puts("\x1b[2;2HSH-1 cycles an instruction, no card\n");
        frame_end();

        /* libyaul's cd_block_init() restarts the CD block, so it goes before the SH-1 start */
        cd_block_init();

        clock_start();

        const bool up = sh1_start();

        dbgio_printf("\x1b[%u;10Hidle      read      xfer\n", ROW_HEADER);
        dbgio_printf("\x1b[%u;2Hkernel  best wall best wall best wall\n", ROW_HEADER + 1);

        for (uint32_t k = 0; k < KERNELS; k++) {
                dbgio_printf("\x1b[%lu;2H%-7s\n", (unsigned long)(ROW_KERNELS + k), kernel_names[k]);
        }

        frame_end();

        if (up) {
                for (uint32_t c = 0; c < CONDITIONS; c++) {
                        condition_run((condition_t)c);
                }
        }

        table_print();

        dbgio_printf("\x1b[%u;2H%-38s\n", ROW_STATUS, "");
        dbgio_printf("\x1b[%u;2H%s\n", ROW_STATUS,
            up ? "Done. empty row: cycles a sample." : "SH-1 not up.");
        frame_end();

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
