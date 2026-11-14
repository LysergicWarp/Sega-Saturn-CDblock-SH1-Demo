/*
 * The CD-block SH-1 demo.
 *
 * Besides its two SH-2 CPUs, every Saturn has a Hitachi SH-1 in the CD block, which runs the CD
 * drive. Its firmware can load extra code from a cartridge in the MPEG card slot. The demo's card
 * carries our own firmware, which lets the SH-2 send the SH-1 jobs.
 *
 * The disc is a benchmark (benchmark.c): one workload of disc decoding and 3D transforms,
 * run with the master alone, then with the slave, the SH-1 and both helping.
 *
 * Bringing the SH-1 up is the dispatch's (src/dispatch/), through the client (src/sh1-client/):
 *
 * 1. E0 asks the CD block to check the MPEG card and load the code on it into the SH-1's memory.
 *    E1 then confirms the card's checks passed, and 01 that an MPEG version is now reported.
 * 2. 93 turns the loaded card firmware on.
 * 3. AD asks the firmware who it is: "DOSE" and its protocol version, so the SH-2 knows it talks
 *    to our firmware and not to a stock MPEG card.
 * 4. The decompress module is uploaded to the firmware, which runs PRS decodes as jobs.
 */

#include <yaul.h>

#include <string.h>

#include <dispatch.h>

#include "benchmark.h"

#define SECTOR_BYTES 2048

/* The decompress module's package, from src/modules-asm, built into the program. */
extern const uint8_t asset_decompress[];
extern const uint8_t asset_decompress_end[];

/* The package is put into the CD block in whole sectors, so its copy is padded to one. */
static uint8_t package[SECTOR_BYTES] __aligned(16);

static benchmark_result_t results[BENCHMARK_RUNS];

/* Ends a frame: the text written so far goes to VDP2, the video chip that draws it, at the next
 * vertical blank. Interrupts are masked across the flush: a vertical blank inside it would leave
 * it unmarked, and libyaul drops every flush after that. */
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

int
main(void)
{
        dbgio_init();
        dbgio_dev_default_init(DBGIO_DEV_VDP2_ASYNC);
        dbgio_dev_font_load();

        dbgio_puts("\x1b[2;2HDisc decode benchmark\n");
        frame_end();

        const uint32_t package_bytes = (uint32_t)(asset_decompress_end - asset_decompress);

        if (package_bytes > sizeof(package)) {
                dbgio_puts("\x1b[5;2HDecompress package larger than its copy\n");
                frame_end();

                while (true) {
                }
        }

        memcpy(package, asset_decompress, package_bytes);

        /* libyaul's CD reads first: its cd_block_init() restarts the CD block, which would turn
         * the card firmware off again */
        cd_block_init();

        const bool found = benchmark_init();

        dispatch_init(package, package_bytes);

        dbgio_printf("\x1b[3;2HSH-1 firmware %s\n",
            dispatch_available(DISPATCH_SH1) ? "up, decoder loaded" : "not loaded, no card");

        dbgio_puts("\x1b[26;2HTimes in emulated seconds.\n");
        frame_end();

        if (!found) {
                dbgio_puts("\x1b[5;2HBENCH.PRS or its table missing or bad\n");
                frame_end();
        } else {
                benchmark_run(5, results);

                uint32_t bad = 0;

                for (uint32_t i = 0; i < BENCHMARK_RUNS; i++) {
                        bad += results[i].chunks_bad;
                }

                dbgio_printf("\x1b[25;2H%s\n",
                    (bad == 0) ? "Every chunk verified" : "Some chunks FAILED");
                frame_end();
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
