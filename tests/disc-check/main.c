/*
 * Decodes a PRS stream that is a file on this disc, STREAM.PRS (../module-check/original.txt
 * compressed by bcl_prs), twice through the dispatch, a line on screen for each step: the SH-1 up,
 * the file found in the disc's file table, then
 * - on the master: the file read into SH-2 memory and decoded there,
 * - on the SH-1: the file read into the CD block's buffer and decoded there, only the output got,
 * - on the master again, libyaul's read now after the client's get.
 * Each with the frames it took, its output compared with the original.
 */

#include <yaul.h>

#include <string.h>

#include <dispatch.h>

#define SECTOR_BYTES 2048

#define PACKAGE_SECTORS 1
#define INPUT_SECTORS 8
#define OUTPUT_SECTORS 10

/* Frames a job may take before the check gives up on it. */
#define JOB_FRAMES 600

/* The disc's root directory holds a handful of files. */
#define FILE_ENTRIES 16

extern const uint8_t asset_decompress[];
extern const uint8_t asset_decompress_end[];
extern const uint8_t asset_original[];
extern const uint8_t asset_original_end[];

static uint8_t package[PACKAGE_SECTORS * SECTOR_BYTES] __aligned(16);
static uint8_t input[INPUT_SECTORS * SECTOR_BYTES] __aligned(16);
static uint8_t output[OUTPUT_SECTORS * SECTOR_BYTES] __aligned(16);

static cdfs_filelist_entry_t entries[FILE_ENTRIES];
static cdfs_filelist_t filelist;

static dispatch_job_t job;

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

/* The stream's entry in the root directory, or NULL. */
static const cdfs_filelist_entry_t *
find_stream(void)
{
        cdfs_config_default_set();
        cdfs_filelist_init(&filelist, entries, FILE_ENTRIES);
        cdfs_filelist_root_read(&filelist);

        for (uint32_t i = 0; i < filelist.entries_count; i++) {
                if ((entries[i].type == CDFS_ENTRY_TYPE_FILE) &&
                    (strcmp(entries[i].name, "STREAM.PRS") == 0)) {
                        return &entries[i];
                }
        }

        return NULL;
}

/* Runs the file's decode on processor and reports it. */
static void
run(const char *step, dispatch_processor_t processor, const cdfs_filelist_entry_t *file)
{
        char detail[24];
        uint32_t frames = 0;

        /* The output not zeros, so a job that writes nothing does not pass */
        memset(input, 0, sizeof(input));
        memset(output, 0xA5, sizeof(output));

        job.input = input;
        job.input_bytes = file->size;
        job.input_fad = file->starting_fad;
        job.output = output;
        job.output_bytes = (uint32_t)(asset_original_end - asset_original);
        job.state = DISPATCH_IDLE;

        const bool submitted = dispatch_submit(processor, &job);

        while ((frames < JOB_FRAMES) && (dispatch_poll(processor) == DISPATCH_RUNNING)) {
                frame_end();
                frames++;
        }

        const bool passed = submitted && (job.state == DISPATCH_DONE) &&
            (memcmp(output, asset_original, job.output_bytes) == 0);

        snprintf(detail, sizeof(detail), "%lu frames", (unsigned long)frames);
        report(step, passed, detail);
}

int
main(void)
{
        dbgio_init();
        dbgio_dev_default_init(DBGIO_DEV_VDP2_ASYNC);
        dbgio_dev_font_load();

        dbgio_puts("\x1b[2;2HDisc decode selftest\n");
        frame_end();

        const uint32_t package_bytes = (uint32_t)(asset_decompress_end - asset_decompress);

        memcpy(package, asset_decompress, package_bytes);
        memset(package + package_bytes, 0, sizeof(package) - package_bytes);

        cd_block_init();

        const cdfs_filelist_entry_t *const file = find_stream();
        const bool found = (file != NULL) && (file->size <= sizeof(input));

        dispatch_init(package, package_bytes);

        report("CD-block SH-1 awake", dispatch_available(DISPATCH_SH1), "");
        report("STREAM.PRS located", found, "");

        if (found) {
                run("Master copy + decode", DISPATCH_MASTER, file);
                run("SH-1 direct decode", DISPATCH_SH1, file);
                /* libyaul's read again, after the client's get */
                run("Master after SH-1", DISPATCH_MASTER, file);
        }

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
