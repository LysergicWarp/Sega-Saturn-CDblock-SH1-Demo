/*
 * Checks the client's transfers by the DMA controller, a line on screen for each step: 48 resets
 * the selector, 64 puts four sectors of a pattern through filter 0, 61 gets them back from
 * partition 0, the words are compared and 62 deletes the sectors. No card needed.
 */

#include <yaul.h>

#include <sh1-client.h>

#define SECTORS 4
#define WORDS (SECTORS * SH1_SECTOR_BYTES / 4)

static uint32_t sent[WORDS] __aligned(16);
static uint32_t received[WORDS] __aligned(16);

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

int
main(void)
{
        dbgio_init();
        dbgio_dev_default_init(DBGIO_DEV_VDP2_ASYNC);
        dbgio_dev_font_load();

        dbgio_puts("\x1b[2;2HDMA transfer check\n");

        for (uint32_t i = 0; i < WORDS; i++) {
                sent[i] = (i << 16) | (~i & 0xFFFF);
                received[i] = 0;
        }

        report("48 reset", sh1_reset_selector());
        report("64 put", sh1_put(0, SECTORS, sent));
        report("61 get", sh1_get(0, 0, SECTORS, received));

        uint32_t mismatch = WORDS;

        for (uint32_t i = 0; i < WORDS; i++) {
                if (received[i] != sent[i]) {
                        mismatch = i;
                        break;
                }
        }

        dbgio_printf("\x1b[%u;2H%s compare     %u/%u equal\n", line, (mismatch == WORDS) ? "PASS" : "FAIL",
            (unsigned)mismatch, (unsigned)WORDS);
        line++;

        report("62 delete", sh1_delete(0, 0, SECTORS));

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
