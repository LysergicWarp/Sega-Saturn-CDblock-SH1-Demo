/*
 * Checks CD-block state lifetime across host reset 04 and the SMPC's CDOFF/CDON. E1 reports the
 * disc result, and 31 and 32 show whether played sectors still reach a partition through the
 * delivery gate. The card checks exercise the same physical reset. SYSRES then restarts the whole
 * program: its second phase checks the reboot marker, clears it and reports the fresh session
 * established by the BIOS. The marker goes through SMEM, so the four bytes the BIOS keeps its
 * settings in end at 0. The reset clears the screen, so the marker also carries how many of the
 * first phase's lines failed, which the second phase reports as its "first phase" line.
 */

#include <yaul.h>

#include <sh1-client.h>

/* The SMPC's registers, through the cache-through area. */
#define SMPC_IREG(n) (*(volatile uint8_t *)(0x20100001UL + 2 * (n)))
#define SMPC_OREG(n) (*(volatile uint8_t *)(0x20100021UL + 2 * (n)))
#define SMPC_COMREG (*(volatile uint8_t *)0x2010001FUL)
#define SMPC_SF (*(volatile uint8_t *)0x20100063UL)

#define SMPC_CDON 0x08
#define SMPC_CDOFF 0x09
#define SMPC_SYSRES 0x0D
#define SMPC_INTBACK 0x10
#define SMPC_SETSMEM 0x17

#define OREG11_CDRES 0x40

#define CD_HIRQ (*(volatile uint16_t *)0x25890008UL)
#define CD_CR(n) (*(volatile uint16_t *)(0x25890018UL + 4 * (n)))

/* Polls before a wait gives up, some seconds. */
#define WAIT_POLLS 0x1000000UL

/* Frames to wait for drive startup, sector delivery and the card's load after CDON. */
#define LOAD_FRAMES 600
#define PLAY_FRAMES 180

/* "1RS" in the first three SMEM bytes. The fourth carries the first phase's failed lines. */
#define REBOOT_MAGIC 0x315253UL

static unsigned line = 4;

/* Lines that failed so far. */
static unsigned failures;

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
report_value(const char *step, bool passed, uint16_t value)
{
        dbgio_printf("\x1b[%u;2H%s %-11s %04X\n", line, passed ? "PASS" : "FAIL", step, value);
        text_flush();
        vdp2_sync();
        vdp2_sync_wait();
        line++;

        if (!passed) {
                failures++;
        }
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

        if (!passed) {
                failures++;
        }
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

/* INTBACK's status block alone, no peripheral data. */
static bool
smpc_status(uint8_t *oreg11)
{
        SMPC_IREG(0) = 0x01;
        SMPC_IREG(1) = 0x00;
        SMPC_IREG(2) = 0xF0;

        if (!smpc_command(SMPC_INTBACK)) {
                return false;
        }

        *oreg11 = SMPC_OREG(11);

        return true;
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

static bool
wait_drive_ready(void)
{
        for (unsigned frames = 0; frames < LOAD_FRAMES; frames++) {
                if (!sh1_command(0x0000, 0x0000, 0x0000, 0x0000, 0)) {
                        return false;
                }

                const uint8_t status = (sh1_answer[0] >> 8) & 0x1F;

                if ((status != 0) && (status != 6) && (status != 7)) {
                        return true;
                }

                wait_frames(1);
        }

        return false;
}

static bool
disc_result(uint8_t expected)
{
        return sh1_command(0xE100, 0x0000, 0x0000, 0x0000, 0) &&
            ((sh1_answer[1] & 0xFF) == expected);
}

/* A closed gate disconnects CD device 0 when the first played sector completes. */
static bool
delivery_gate(bool open)
{
        if (!sh1_reset_selector() ||
            !sh1_command(0x3000, 0x0000, 0x0000, 0x0000, SH1_ESEL) ||
            !sh1_command(0x1080, 150, 0x0080, 5, 0)) {
                return false;
        }

        wait_frames(PLAY_FRAMES);

        if (!sh1_command(0x3200, 0x0000, 0x0000, 0x0000, 0)) {
                return false;
        }

        const uint8_t expected = open ? 0 : 0xFF;
        const bool partition_ok = ((sh1_answer[2] >> 8) == expected);

        return sh1_command(0x3100, 0x0000, 0x0000, 0x0000, 0) && partition_ok &&
            ((sh1_answer[2] >> 8) == expected);
}

static bool
card_identity(uint16_t activations)
{
        return sh1_command(0xAD00, 0x0000, 0x0000, 0x0000, 0) && (sh1_answer[1] == activations) &&
            (sh1_answer[2] == 0x444F) && (sh1_answer[3] == 0x5345);
}

static void
sysres_return(unsigned first_failures)
{
        dbgio_puts("\x1b[2;2HCD block SYSRES return\n");
        report_value("reboot marker", true, 1);

        /* The reset cleared the first phase's lines, so their failures come with the marker. */
        report_value("first phase", first_failures == 0, first_failures);

        /* SMEM persists, so the next run starts at the first phase only once the marker is gone. */
        for (unsigned i = 0; i < 4; i++) {
                SMPC_IREG(i) = 0x00;
        }

        report_value("SMEM clear", smpc_command(SMPC_SETSMEM), 0);

        /* The BIOS authenticates and reads the disc again before this program returns. */
        report("fresh E1 4", disc_result(4));
        report("fresh gate", delivery_gate(true));

        while (true) {
        }
}

int
main(void)
{
        uint8_t oreg11 = 0;
        bool passed = smpc_status(&oreg11);
        uint32_t marker = 0;

        for (unsigned i = 0; i < 4; i++) {
                marker = (marker << 8) | SMPC_OREG(12 + i);
        }

        const bool returned = passed && ((marker >> 8) == REBOOT_MAGIC);

        dbgio_init();
        dbgio_dev_default_init(DBGIO_DEV_VDP2_ASYNC);
        dbgio_dev_font_load();

        if (returned) {
                sysres_return(marker & 0xFF);
        }

        dbgio_puts("\x1b[2;2HCD block reset check\n");

        report("initial E1 4", disc_result(4));
        report("04 reset", sh1_command(0x0401, 0x0000, 0x0000, 0x0000, 0));
        wait_frames(30);
        report("04 E1 4", disc_result(4));
        report("04 gate open", delivery_gate(true));

        passed = smpc_status(&oreg11) && ((oreg11 & OREG11_CDRES) != 0);
        report_value("CDRES high", passed, oreg11);

        passed = sh1_card_load() && sh1_card_on() && card_identity(1);
        report("card on", passed);

        report_value("CDOFF", smpc_command(SMPC_CDOFF), 0);

        oreg11 = 0xFF;
        passed = smpc_status(&oreg11) && ((oreg11 & OREG11_CDRES) == 0);
        report_value("CDRES low", passed, oreg11);

        /* What the interface chip answers in reset is not known. The fork's model reads 0. */
        const uint16_t flags = CD_HIRQ;
        report_value("HIRQ held", flags == 0, flags);

        report_value("CDON", smpc_command(SMPC_CDON), 0);
        report("signature", wait_signature());

        oreg11 = 0;
        passed = smpc_status(&oreg11) && ((oreg11 & OREG11_CDRES) != 0);
        report_value("CDRES high", passed, oreg11);

        report("hard E1 0", disc_result(0));
        report("drive ready", wait_drive_ready());
        report("hard gate shut", delivery_gate(false));

        passed = sh1_command(0xAD00, 0x0000, 0x0000, 0x0000, 0) &&
            ((sh1_answer[0] >> 8) == SH1_STATUS_REJECT);
        report("AD card off", passed);

        unsigned frames = 0;

        while (!sh1_card_load() && (frames < LOAD_FRAMES)) {
                wait_frames(1);
                frames++;
        }

        passed = (frames < LOAD_FRAMES) && sh1_card_on() && card_identity(1);
        report("card again", passed);
        report_value("load frames", frames < LOAD_FRAMES, frames);

        /* The marker, and with it the lines that failed up to here, 255 at most. */
        const uint32_t marker_out = (REBOOT_MAGIC << 8) | ((failures < 255) ? failures : 255);

        for (unsigned i = 0; i < 4; i++) {
                SMPC_IREG(i) = (marker_out >> (24 - 8 * i)) & 0xFF;
        }

        report_value("SMEM marker", smpc_command(SMPC_SETSMEM), 0);
        report_value("SYSRES", smpc_command(SMPC_SYSRES), 0);

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
