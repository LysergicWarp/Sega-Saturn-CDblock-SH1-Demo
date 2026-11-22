/*
 * The disc decode benchmark with the card firmware off the disc, for a console with no MPEG card.
 *
 * The benchmark of src/demo-discs/demo (benchmark.c): one workload of disc decoding and 3D
 * transforms, run with the master alone, then with the slave, the SH-1 and both helping. Here the
 * SH-1 code comes from this disc. Its header names src/disc-extensions/firmware-boot-asm as its
 * disc extension: after each accepted disc authentication the CD block runs it, and it installs the
 * card firmware where a card load would and marks a card present.
 *
 * Bringing the SH-1 up:
 *
 * 1. E0 authenticates the disc again, so the extension runs for this disc whatever ran before it.
 *    E1 then gives the result, 4 for an accepted disc, and 01 the CD block revision the extension
 *    sets (B0) and the hardware flag, with bit 1 now set. Line 3 shows the three.
 * 2. The dispatch, built with DISPATCH_FIRMWARE_FROM_DISC, skips the card load: 93 turns the
 *    firmware on, AD checks it is ours, and the decompress module is uploaded.
 */

#include <yaul.h>

#include <string.h>

#include <dispatch.h>
#include <sh1-client.h>

#include "benchmark.h"

#define SECTOR_BYTES 2048

/* The CD block's interrupt flags register, for E0's end, EFLS, which the client does not wait for. */
#define CD_HIRQ (*(volatile uint16_t *)0x25890008UL)
#define HIRQ_EFLS 0x0200

/* Frames E0 may take, past the firmware's own 20 s limit on the disc check. */
#define AUTH_FRAMES 1500

/* The CD block revision the disc extension sets, its byte +0x44. */
#define EXTENSION_REVISION 0xB0

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

/* E0 subcode 0 on partition 0, as the BIOS sends it, then its end, EFLS, a frame at a time. False
 * when E0 is rejected or EFLS never comes. */
static bool
authenticate(void)
{
        CD_HIRQ = (uint16_t)~HIRQ_EFLS;

        if (!sh1_command(0xE000, 0x0000, 0x0000, 0x0000, 0) ||
            ((sh1_answer[0] >> 8) == SH1_STATUS_REJECT)) {
                return false;
        }

        for (uint32_t frames = 0; frames < AUTH_FRAMES; frames++) {
                if ((CD_HIRQ & HIRQ_EFLS) != 0) {
                        return true;
                }

                frame_end();
        }

        return false;
}

/* The firmware put in place by the disc's extension: E0, then E1's result and 01's revision and
 * hardware flag, shown on line 3. True when the extension ran and set the card bit. */
static bool
firmware_from_disc(void)
{
        uint8_t result = 0xFF;
        uint8_t revision = 0xFF;
        uint8_t flag = 0xFF;

        dbgio_puts("\x1b[3;2HE0, disc check\n");
        frame_end();

        const bool authenticated = authenticate();

        if (sh1_command(0xE100, 0x0000, 0x0000, 0x0000, 0) &&
            ((sh1_answer[0] >> 8) != SH1_STATUS_REJECT)) {
                result = sh1_answer[1] & 0xFF;
        }

        if (sh1_command(0x0100, 0x0000, 0x0000, 0x0000, 0) &&
            ((sh1_answer[0] >> 8) != SH1_STATUS_REJECT)) {
                flag = sh1_answer[1] >> 8;
                revision = sh1_answer[3] & 0xFF;
        }

        const bool placed = authenticated && (revision == EXTENSION_REVISION) &&
            ((flag & 0x02) != 0);

        dbgio_printf("\x1b[3;2HE1 %02X rev %02X hw %02X %s\n", result, revision, flag,
            !authenticated ? "E0 failed" : (placed ? "" : "no ext"));
        frame_end();

        return placed;
}

int
main(void)
{
        dbgio_init();
        dbgio_dev_default_init(DBGIO_DEV_VDP2_ASYNC);
        dbgio_dev_font_load();

        dbgio_puts("\x1b[2;2HDisc decode benchmark, no card\n");
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

        /* Without the firmware in place the dispatch's 93 is rejected, the SH-1 is left out, and
         * its runs say so */
        (void)firmware_from_disc();

        dispatch_init(package, package_bytes);

        dbgio_printf("\x1b[3;30H%s\n", dispatch_available(DISPATCH_SH1) ? "SH-1 up" : "SH-1 down");

        dbgio_puts("\x1b[26;2HTimes in seconds, counted in frames.\n");
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
