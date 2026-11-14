/*
 * Takes the card firmware and its decompress module through their lifetime, a line on screen for
 * each case: a stop during a job, 04, an unload, a reload, a disc accepted after a stop, a cold
 * start by the SMPC's CDOFF and CDON, the handles coming round after 248 loads, and the load after
 * that going on from the next handle. After a case that turns the card off or loses the firmware,
 * the firmware is found again and the module loaded again when the case forgets it. Every case
 * ends with a job that decodes a PRS stream of original.txt into sectors that are got back and
 * compared with the original. A failed case shows its first failing step under it, with the answer
 * to that step's last command.
 */

#include <yaul.h>

#include <string.h>

#include <sh1-client.h>

/* The SMPC's registers, through the cache-through area. */
#define SMPC_COMREG (*(volatile uint8_t *)0x2010001FUL)
#define SMPC_SF (*(volatile uint8_t *)0x20100063UL)

#define SMPC_CDON 0x08
#define SMPC_CDOFF 0x09

#define CD_HIRQ (*(volatile uint16_t *)0x25890008UL)
#define CD_CR(n) (*(volatile uint16_t *)(0x25890018UL + 4 * (n)))

/* The end of a disc check. */
#define CD_EFLS 0x0200

/* Polls before a wait gives up, some seconds. */
#define WAIT_POLLS 0x1000000UL

/* 51s before the wait for a job's first sector gives up. */
#define FILE_POLLS 0x10000UL

/* Frames to wait after 04, after a stop before its output is counted again, for a card load and
 * for a disc check. */
#define RESTART_FRAMES 30
#define STOPPED_FRAMES 30
#define LOAD_FRAMES 600
#define DISC_FRAMES 1800

/* The partitions, each filled through the filter of the same number. The disc check reads into
 * DISC. */
#define INPUT 0
#define OUTPUT 1
#define DISC 2

#define JOB_ID 1

/* The buffer's slots. Those the input leaves free bound the output. */
#define SLOTS 200

#define PUT_SECTORS 8
#define GOT_SECTORS 10

/* The handles after a load of the firmware start here and come round after this many loads. */
#define HANDLE_FIRST 3
#define HANDLE_LAST 250
#define HANDLE_ROUND 248

extern const uint8_t asset_decompress[];
extern const uint8_t asset_decompress_end[];
extern const uint8_t asset_stream[];
extern const uint8_t asset_stream_end[];
extern const uint8_t asset_original[];
extern const uint8_t asset_original_end[];

static uint8_t put_buffer[PUT_SECTORS * SH1_SECTOR_BYTES] __aligned(16);
static uint8_t got[GOT_SECTORS * SH1_SECTOR_BYTES] __aligned(16);

static unsigned line = 4;

/* The case's first failing step and the answer to its last command. */
static const char *failed_step;
static uint16_t failed_answer[4];

/* Keeps the case's first failure. Returns passed. */
static bool
step(const char *name, bool passed)
{
        if (!passed && (failed_step == NULL)) {
                failed_step = name;
                memcpy(failed_answer, sh1_answer, sizeof(failed_answer));
        }

        return passed;
}

/* The case's line with a value, then its first failure, which it clears. */
static void
report(const char *name, const char *label, unsigned value)
{
        dbgio_printf("\x1b[%u;2H%s %-11s %s %u\n", line, (failed_step == NULL) ? "PASS" : "FAIL",
            name, label, value);
        line++;

        if (failed_step != NULL) {
                dbgio_printf("\x1b[%u;4H%-15s %04X %04X %04X %04X\n", line, failed_step,
                    failed_answer[0], failed_answer[1], failed_answer[2], failed_answer[3]);
                line++;
                failed_step = NULL;
        }

        /* Interrupts masked across the flush: a vertical blank inside it would leave it unmarked,
         * and libyaul drops every flush after that */
        const uint32_t mask = cpu_intc_mask_get();

        cpu_intc_mask_set(15);
        dbgio_flush();
        cpu_intc_mask_set(mask);

        vdp2_sync();
        vdp2_sync_wait();
}

static void
wait_frames(unsigned frames)
{
        for (unsigned i = 0; i < frames; i++) {
                vdp2_sync();
                vdp2_sync_wait();
        }
}

static bool
wait_sf_clear(void)
{
        for (uint32_t i = 0; i < WAIT_POLLS; i++) {
                if ((SMPC_SF & 1) == 0) {
                        return true;
                }
        }

        return false;
}

static bool
smpc_command(uint8_t command)
{
        if (!wait_sf_clear()) {
                return false;
        }

        SMPC_SF = 1;
        SMPC_COMREG = command;

        return wait_sf_clear();
}

/* The NUL CDBLOCK signature in CR1..CR4, which startup publishes. */
static bool
wait_signature(void)
{
        for (uint32_t i = 0; i < WAIT_POLLS; i++) {
                if ((CD_CR(0) == 0x0043) && (CD_CR(1) == 0x4442) && (CD_CR(2) == 0x4C4F) &&
                    (CD_CR(3) == 0x434B)) {
                        for (uint32_t n = 0; n < 4; n++) {
                                sh1_answer[n] = CD_CR(n);
                        }

                        return true;
                }
        }

        return false;
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

/* A module job's argument: CR3 the input partition and the output filter, CR4 the most output
 * sectors. */
static uint32_t
argument(uint16_t limit)
{
        return ((uint32_t)INPUT << 24) | ((uint32_t)OUTPUT << 16) | limit;
}

static bool
delete_both(void)
{
        return sh1_delete(INPUT, 0, 0xFFFF) && sh1_delete(OUTPUT, 0, 0xFFFF);
}

/* 51: the sectors in partition, or 0xFFFF when the command fails. */
static uint16_t
sectors_in(uint8_t partition)
{
        if (!sh1_command(0x5100, 0x0000, (uint16_t)(partition << 8), 0x0000, 0)) {
                return 0xFFFF;
        }

        return sh1_answer[3];
}

static bool
rejected(uint8_t handle)
{
        return !sh1_submit(handle, JOB_ID, argument(1)) &&
            ((sh1_answer[0] >> 8) == SH1_STATUS_REJECT);
}

/* AD needs the card bit, so it is rejected while the card is off. */
static bool
card_off(void)
{
        return sh1_command(0xAD00, 0x0000, 0x0000, 0x0000, 0) &&
            ((sh1_answer[0] >> 8) == SH1_STATUS_REJECT);
}

static bool
zeros(const uint8_t *bytes, uint32_t count)
{
        for (uint32_t i = 0; i < count; i++) {
                if (bytes[i] != 0) {
                        return false;
                }
        }

        return true;
}

/* Loads the module: its handle, or 0 when the load fails. */
static uint8_t
load(void)
{
        sh1_job_t job = { 0 };
        const uint16_t sectors = stage(asset_decompress, asset_decompress_end);

        if ((sectors == 0) || !sh1_module_load(INPUT, put_buffer, sectors, JOB_ID, &job)) {
                return 0;
        }

        return (uint8_t)job.result;
}

/* A job of the module: the stream put, decoded into the output partition, got back and compared
 * with the original and the zeros after it, then both partitions deleted. A wait that gives up
 * leaves them, since the job may still be using them. */
static void
run_job(uint8_t handle)
{
        const uint32_t original = (uint32_t)(asset_original_end - asset_original);
        const uint16_t filed = (uint16_t)((original + SH1_SECTOR_BYTES - 1) / SH1_SECTOR_BYTES);
        const uint16_t sectors = stage(asset_stream, asset_stream_end);
        sh1_job_t job = { 0 };

        step("put stream", (sectors != 0) && sh1_put(INPUT, sectors, put_buffer));

        const bool submitted = sh1_submit(handle, JOB_ID, argument(SLOTS - sectors));
        const bool ended = submitted && sh1_wait(JOB_ID, &job);
        step("decompress", ended && (job.state == SH1_JOB_DONE) && (job.result == original));

        if (submitted && !ended) {
                return;
        }

        const bool passed = (filed <= GOT_SECTORS) && sh1_get(OUTPUT, 0, filed, got) &&
            (memcmp(got, asset_original, original) == 0) &&
            zeros(got + original, (uint32_t)filed * SH1_SECTOR_BYTES - original);
        step("get output", passed);

        step("delete", delete_both());
}

/* Starts a job of the module and stops the card with 93 once the job has filed its first sector,
 * then waits for the stop's MPED. The job's own MPED must not have come before, the card must be
 * off after, and the sectors filed must stay as they were. Both partitions are then deleted, unless
 * the stop's MPED never came. Returns the sectors filed, fewer than the job's when the stop came
 * during it. */
static uint16_t
stop_during_job(uint8_t handle)
{
        const uint32_t original = (uint32_t)(asset_original_end - asset_original);
        const uint16_t sectors = stage(asset_stream, asset_stream_end);
        uint16_t filed = 0;

        step("put stream", (sectors != 0) && sh1_put(INPUT, sectors, put_buffer));
        step("submit", sh1_submit(handle, JOB_ID, argument(SLOTS - sectors)));

        /* 0xFFFF, no answer, ends the polls too */
        for (uint32_t i = 0; (i < FILE_POLLS) && (filed == 0); i++) {
                filed = sectors_in(OUTPUT);
        }

        step("first sector", (filed != 0) && (filed != 0xFFFF));

        /* sh1_submit cleared MPED, and nothing but the job's end sets it before the stop */
        step("job out", (CD_HIRQ & SH1_MPED) == 0);

        if (!step("93 stop", sh1_command(0x9300, 0x0000, 0x0000, 0x0000, SH1_MPED))) {
                return filed;
        }

        step("AD card off", card_off());

        filed = sectors_in(OUTPUT);
        step("stopped in job",
            (filed != 0) && (filed < (original + SH1_SECTOR_BYTES - 1) / SH1_SECTOR_BYTES));

        /* No pass runs after the stop's MPED */
        wait_frames(STOPPED_FRAMES);
        step("none after", sectors_in(OUTPUT) == filed);
        step("delete", delete_both());

        return filed;
}

/* E0 subcode 0 checks the disc, reading into partition DISC, and EFLS ends it. True when E1 then
 * gives result 4, the disc accepted. */
static bool
disc_accepted(void)
{
        CD_HIRQ = (uint16_t)~CD_EFLS;

        /* A rejected command's status ff has the wait bit too */
        if (!sh1_command(0xE000, 0x0000, (uint16_t)(DISC << 8), 0x0000, 0) ||
            (((sh1_answer[0] >> 8) & SH1_STATUS_WAIT) != 0)) {
                return false;
        }

        for (unsigned frames = 0; (CD_HIRQ & CD_EFLS) == 0; frames++) {
                if (frames == DISC_FRAMES) {
                        return false;
                }

                wait_frames(1);
        }

        return sh1_command(0xE100, 0x0000, 0x0000, 0x0000, 0) && ((sh1_answer[1] & 0xFF) == 4);
}

/* Finds the firmware again: 01's MPEG version, nonzero while the firmware is loaded and expected as
 * loaded says. When 01 answers 0, E0 subcode 1, again each frame for a while. Then 93 and AD with
 * the activations expected. Returns the activations AD gives. */
static uint16_t
find_again(bool loaded, uint16_t activations)
{
        const bool answered = sh1_command(0x0100, 0x0000, 0x0000, 0x0000, 0);
        const bool found = answered && ((sh1_answer[2] & 0xFF) != 0);

        step("01 version", answered && (found == loaded));

        if (answered && !found) {
                unsigned frames = 0;

                while (!sh1_card_load() && (frames < LOAD_FRAMES)) {
                        wait_frames(1);
                        frames++;
                }

                step("E0 E1 load", frames < LOAD_FRAMES);
        }

        step("93 card on", sh1_card_on());

        uint16_t seen = 0;
        step("AD identity", sh1_identity(&seen) && (seen == activations));

        return seen;
}

/* After a case that forgets the module: the old handle rejected, the module loaded again with the
 * handle expected, and its job. Returns the handle the load gave. */
static uint8_t
module_again(uint8_t old, uint8_t expected)
{
        step("old handle", rejected(old));

        const uint8_t handle = load();
        step("load", handle == expected);

        run_job(expected);

        return handle;
}

int
main(void)
{
        dbgio_init();
        dbgio_dev_default_init(DBGIO_DEV_VDP2_ASYNC);
        dbgio_dev_font_load();

        dbgio_puts("\x1b[2;2HLifecycle check\n");

        /* The firmware loaded and on, the module's first handle */
        (void)find_again(false, 1);
        step("48 reset", sh1_reset_selector());
        report("setup", "handle", module_again(HANDLE_FIRST, HANDLE_FIRST));

        /* 93 ends the job between passes, and the module stays */
        const uint16_t filed = stop_during_job(HANDLE_FIRST);
        (void)find_again(true, 2);
        run_job(HANDLE_FIRST);
        report("stop", "filed", filed);

        /* 04 restarts the CD block's tasks and keeps the firmware and the module */
        step("04 restart", sh1_command(0x0401, 0x0000, 0x0000, 0x0000, 0));
        wait_frames(RESTART_FRAMES);
        step("AD card off", card_off());
        step("48 reset", sh1_reset_selector());
        const uint16_t activations = find_again(true, 3);
        run_job(HANDLE_FIRST);
        report("04", "activations", activations);

        /* An unload, after which the handle is rejected and the next load gives the next one */
        sh1_job_t job = { 0 };
        bool passed = sh1_submit(SH1_OP_UNLOAD, JOB_ID, 0) && sh1_wait(JOB_ID, &job) &&
            (job.state == SH1_JOB_DONE) && (job.result == 0);
        step("unload", passed);
        report("unload", "handle", module_again(HANDLE_FIRST, HANDLE_FIRST + 1));

        /* A reload after a stop forgets the module, and the handles start again */
        step("93 stop", sh1_command(0x9300, 0x0000, 0x0000, 0x0000, SH1_MPED));
        step("E0 E1 load", sh1_card_load());
        (void)find_again(true, 1);
        report("reload", "handle", module_again(HANDLE_FIRST + 1, HANDLE_FIRST));

        /* A disc accepted after a stop clears the firmware and the module */
        (void)stop_during_job(HANDLE_FIRST);
        step("disc accepted", disc_accepted());
        step("48 reset", sh1_reset_selector());
        (void)find_again(false, 1);
        report("disc", "handle", module_again(HANDLE_FIRST, HANDLE_FIRST));

        /* CDOFF and CDON reset the whole CD block */
        step("CDOFF", smpc_command(SMPC_CDOFF));

        /* The CD block held in reset for a frame */
        wait_frames(1);
        step("CDON", smpc_command(SMPC_CDON));
        step("signature", wait_signature());
        (void)find_again(false, 1);
        step("48 reset", sh1_reset_selector());
        report("cold start", "handle", module_again(HANDLE_FIRST, HANDLE_FIRST));

        /* The handles from 4 to 250, then 3 again, and 250 rejected */
        uint8_t handle = HANDLE_FIRST;
        unsigned loads = 0;

        while (loads < HANDLE_ROUND) {
                const uint8_t next = (handle == HANDLE_LAST) ? HANDLE_FIRST : (uint8_t)(handle + 1);

                if (load() != next) {
                        break;
                }

                handle = next;
                loads++;
        }

        step("loads", (loads == HANDLE_ROUND) && (handle == HANDLE_FIRST));
        step("old handle", rejected(HANDLE_LAST));
        run_job(HANDLE_FIRST);
        report("wrap", "loads", loads);

        /* The next load after them gives 4, and 3 is rejected */
        handle = load();
        step("load", handle == HANDLE_FIRST + 1);
        step("old handle", rejected(HANDLE_FIRST));
        run_job(HANDLE_FIRST + 1);
        report("after wrap", "handle", handle);

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
