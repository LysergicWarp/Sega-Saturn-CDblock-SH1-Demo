/*
 * The shell's screen. The bitmap's setup follows libyaul's vdp2-normal-bitmap example, the pad's
 * reading its dbgio-menu example.
 *
 * VDP2, the Saturn's background video chip, stacks up to five background layers, the scroll
 * screens NBG0 to NBG3 and RBG0, each a map of tiles or a bitmap, over a single back color. Here:
 * - NBG0, the game's bitmap: 512 by 256 pixels of 15-bit color, VRAM banks A0 and A1.
 * - NBG3, libyaul's text layer (dbgio), 8 by 8 pixel characters in bank B1, on top. Its unused
 *   pixels are see-through.
 *
 * VDP2's memory (VRAM) is four banks of 128 KB. While the screen is drawn, each bank takes eight
 * accesses in turn, over and over, and the program says who gets which of the eight slots, the
 * bank's cycle pattern. A 15-bit bitmap needs four in each bank it sits in. The CPU may then only
 * reach VRAM in the slots given to it, so the shell gives it the bitmap's spare ones.
 */

#include <yaul.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "shell.h"

/* The text rows, counted from 1 as dbgio's cursor moves count them, and their columns. */
#define ROW_NAME 2
#define ROW_DESCRIPTION 3
#define ROW_STATUS 4
#define COLUMN_FIRST 2
#define COLUMNS 38

/* The text rows' band, 4 rows of 8 pixels, above the game's area. */
#define BAND_HEIGHT 32
#define BAND_COLOR RGB1555(1, 0, 0, 7)

#define BITMAP ((uint16_t *)VDP2_VRAM_ADDR(0, 0x00000))
#define BITMAP_HEIGHT 256

_Static_assert(BAND_HEIGHT + SHELL_HEIGHT == 224, "the band and the game's area fill the screen");

/* Auto mode flips the toggle every 600 frames: 10 s at 60 frames a second, 12 s on a 50 Hz
 * (PAL) console. */
#define AUTO_FRAMES 600

static const shell_game_t *game_shown;
static char status[COLUMNS + 1];

static bool toggle;
static bool automatic;
static uint32_t flipped;

static smpc_peripheral_digital_t pad;
static bool a_before;

/* Frames since shell_init(), counted at each vertical blank's start. */
static volatile uint32_t frames;

static void
vblank_in(void *work __unused)
{
        frames++;
}

/* At each vertical blank's end the SMPC, the chip that reads the pads, is asked for their state,
 * which smpc_peripheral_process() collects in the frame that follows. */
static void
vblank_out(void *work __unused)
{
        smpc_peripheral_intback_issue();
}

static void
bitmap_init(void)
{
        const vdp2_scrn_bitmap_format_t format = {
                .scroll_screen = VDP2_SCRN_NBG0,
                .ccc = VDP2_SCRN_CCC_RGB_32768,
                .bitmap_size = VDP2_SCRN_BITMAP_SIZE_512X256,
                .palette_base = 0x00000000,
                .bitmap_base = VDP2_VRAM_ADDR(0, 0x00000)
        };

        vdp2_scrn_bitmap_format_set(&format);

        /* Under the text layer, which dbgio puts at priority 7, and with it kept on */
        vdp2_scrn_priority_set(VDP2_SCRN_NBG0, 3);
        vdp2_scrn_display_set(vdp2_scrn_display_get() | VDP2_SCRN_DISP_NBG0);

        /* The same slots in A0 and A1: the bitmap's four reads, then an empty slot, which the
         * VDP2 manual asks for before the CPU's slots when a VRAM is split in two banks, then
         * the CPU's three. B1's slots are dbgio's, set when it starts. */
        const vdp2_vram_cycp_bank_t slots = {
                .t0 = VDP2_VRAM_CYCP_CHPNDR_NBG0,
                .t1 = VDP2_VRAM_CYCP_CHPNDR_NBG0,
                .t2 = VDP2_VRAM_CYCP_CHPNDR_NBG0,
                .t3 = VDP2_VRAM_CYCP_CHPNDR_NBG0,
                .t4 = VDP2_VRAM_CYCP_NO_ACCESS,
                .t5 = VDP2_VRAM_CYCP_CPU_RW,
                .t6 = VDP2_VRAM_CYCP_CPU_RW,
                .t7 = VDP2_VRAM_CYCP_CPU_RW
        };

        vdp2_vram_cycp_bank_set(VDP2_VRAM_BANK_A0, &slots);
        vdp2_vram_cycp_bank_set(VDP2_VRAM_BANK_A1, &slots);

        /* Scrolled down so the bitmap's row 0 shows on the screen's line BAND_HEIGHT. A bitmap
         * screen repeats in every direction, so the screen's top lines show the bitmap's last
         * rows, painted in the band's color. The game never sees them. */
        vdp2_scrn_scroll_y_set(VDP2_SCRN_NBG0, (fix16_t)((BITMAP_HEIGHT - BAND_HEIGHT) << 16));
}

/* The band's rows in its color, the rest black, two pixels a write. */
static void
bitmap_paint(void)
{
        uint32_t *const pairs = (uint32_t *)BITMAP;
        const uint32_t band = ((uint32_t)BAND_COLOR.raw << 16) | BAND_COLOR.raw;
        const uint32_t black = ((uint32_t)RGB1555(1, 0, 0, 0).raw << 16) | RGB1555(1, 0, 0, 0).raw;

        for (uint32_t y = 0; y < BITMAP_HEIGHT; y++) {
                const uint32_t pair = (y >= BITMAP_HEIGHT - BAND_HEIGHT) ? band : black;

                for (uint32_t x = 0; x < SHELL_BITMAP_WIDTH / 2; x++) {
                        pairs[y * (SHELL_BITMAP_WIDTH / 2) + x] = pair;
                }
        }
}

/* One text row: left from its first column, right ending at its last, spaces between. */
static void
row_write(unsigned row, const char *left, const char *right)
{
        char line[COLUMNS + 1];

        memset(line, ' ', COLUMNS);
        line[COLUMNS] = '\0';

        size_t length = strlen(left);

        memcpy(line, left, (length < COLUMNS) ? length : COLUMNS);

        if (right != NULL) {
                length = strlen(right);

                if (length <= COLUMNS) {
                        memcpy(line + COLUMNS - length, right, length);
                }
        }

        dbgio_printf("\x1b[%u;%uH%s\n", row, COLUMN_FIRST, line);
}

static void
rows_write(void)
{
        char indicator[COLUMNS + 1];
        const char *right = NULL;

        if (game_shown->toggle_labels[0] != NULL) {
                (void)snprintf(indicator, sizeof(indicator), "A: %s%s",
                    game_shown->toggle_labels[toggle ? 1 : 0], automatic ? " (auto)" : "");
                right = indicator;
        }

        row_write(ROW_NAME, game_shown->name, right);
        row_write(ROW_DESCRIPTION, game_shown->description, NULL);
        row_write(ROW_STATUS, status, NULL);
}

void
shell_init(const shell_game_t *game)
{
        game_shown = game;
        automatic = (game->toggle_labels[0] != NULL);

        vdp2_tvmd_display_res_set(VDP2_TVMD_INTERLACE_NONE, VDP2_TVMD_HORZ_NORMAL_A,
            VDP2_TVMD_VERT_224);
        vdp2_scrn_back_color_set(VDP2_VRAM_ADDR(3, 0x01FFFE), BAND_COLOR);

        /* dbgio after the resolution, which sets its rows and columns */
        dbgio_init();
        dbgio_dev_default_init(DBGIO_DEV_VDP2_ASYNC);
        dbgio_dev_font_load();

        bitmap_init();

        smpc_peripheral_init();
        vdp_sync_vblank_in_set(vblank_in, NULL);
        vdp_sync_vblank_out_set(vblank_out, NULL);

        vdp2_tvmd_display_set();

        /* The settings above reach VDP2 at the next vertical blank. The bitmap is painted after,
         * once the CPU has its slots */
        vdp2_sync();
        vdp2_sync_wait();

        bitmap_paint();
}

uint16_t *
shell_bitmap(void)
{
        return BITMAP;
}

void
shell_scroll(uint32_t x)
{
        vdp2_scrn_scroll_x_set(VDP2_SCRN_NBG0, (fix16_t)((x % SHELL_BITMAP_WIDTH) << 16));
}

void
shell_status(const char *format, ...)
{
        va_list arguments;

        va_start(arguments, format);
        (void)vsnprintf(status, sizeof(status), format, arguments);
        va_end(arguments);
}

bool
shell_toggle(void)
{
        return toggle;
}

void
shell_frame(void)
{
        smpc_peripheral_process();
        smpc_peripheral_digital_port(1, &pad);

        const bool a = pad.connected && (pad.pressed.button.a != 0);

        if (game_shown->toggle_labels[0] != NULL) {
                /* A press flips it once however long A is held, and ends auto mode */
                if (a && !a_before) {
                        toggle = !toggle;
                        automatic = false;
                } else if (automatic && ((frames - flipped) >= AUTO_FRAMES)) {
                        toggle = !toggle;
                        flipped = frames;
                }
        }

        a_before = a;

        rows_write();

        /* The text to VDP2, and the frame's register changes (the scroll among them) applied at
         * the next vertical blank, which this waits for. Interrupts are masked across the flush: a
         * vertical blank inside it would leave it unmarked, and libyaul drops every flush after
         * that */
        const uint32_t mask = cpu_intc_mask_get();

        cpu_intc_mask_set(15);
        dbgio_flush();
        cpu_intc_mask_set(mask);

        vdp2_sync();
        vdp2_sync_wait();
}
