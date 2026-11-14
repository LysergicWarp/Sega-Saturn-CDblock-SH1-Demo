/*
 * Loads the MPEG card's firmware and checks it, a line on screen for each step: E0 subcode 1 and
 * E1 load the card, 01 gives its MPEG version, AD is rejected while the card is off, 93 turns it
 * on, AD gives the firmware's identity and AB runs a ping job that AC reports.
 */

#include <yaul.h>

#include <sh1-client.h>

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

        dbgio_puts("\x1b[2;2HMPEG card firmware check\n");

        report("E0 E1 load", sh1_card_load());

        bool passed = sh1_command(0x0100, 0x0000, 0x0000, 0x0000, 0) && ((sh1_answer[2] & 0xFF) == 1);
        report("01 version", passed);

        passed = sh1_command(0xAD00, 0x0000, 0x0000, 0x0000, 0) && ((sh1_answer[0] >> 8) == SH1_STATUS_REJECT);
        report("AD card off", passed);

        report("93 card on", sh1_card_on());

        passed = sh1_command(0xAD00, 0x0000, 0x0000, 0x0000, 0) && (sh1_answer[1] == 1) &&
            (sh1_answer[2] == 0x444F) && (sh1_answer[3] == 0x5345);
        report("AD identity", passed);

        report("AB ping", sh1_command(0xAB00, 0x1234, 0x89AB, 0xCDEF, SH1_MPED));

        passed = sh1_command(0xAC00, 0x0000, 0x0000, 0x0000, 0) && ((sh1_answer[0] & 0xFF) == 3) &&
            (sh1_answer[1] == 0x1234) && (sh1_answer[2] == 0x7654) && (sh1_answer[3] == 0x3210);
        report("AC ping", passed);

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
