/*
 * Disc extension takeover check. This disc's header names an SH-1 extension that copies the ROM's
 * vector table, wraps IRQ6 and hooks task 6's message resumption. Both wrappers forward to the ROM.
 * Command 01's CR4 low byte reports task messages in its high nibble and IRQ6 entries in its low
 * nibble, both modulo 16. Each line also shows E1's result after one step: the BIOS's boot, a host
 * reset (04), two more disc authentications (E0), a CD block power cycle and one more E0.
 */

#include <yaul.h>

#define CD_HIRQ (*(volatile uint16_t *)0x25890008UL)
#define CD_CR(n) (*(volatile uint16_t *)(0x25890018UL + 4 * (n)))

#define HIRQ_CMOK 0x0001
#define HIRQ_EFLS 0x0200

#define SMPC_SF (*(volatile uint8_t *)0x20100063UL)
#define SMPC_COMREG (*(volatile uint8_t *)0x2010001FUL)

#define SMPC_CDON 0x08
#define SMPC_CDOFF 0x09

/* Polls before a wait gives up, some seconds. */
#define WAIT_POLLS 0x1000000UL

/* Frames an E0 may take, past the firmware's own 20 s timeout, and the drive's startup. */
#define AUTH_FRAMES 1500
#define READY_FRAMES 600

static uint16_t answer[4];
static unsigned line = 4;

static void
wait_frames(unsigned frames)
{
        for (unsigned i = 0; i < frames; i++) {
                vdp2_sync();
                vdp2_sync_wait();
        }
}

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
print(const char *text)
{
        dbgio_printf("\x1b[%u;2H%s\n", line, text);
        text_flush();
        vdp2_sync();
        vdp2_sync_wait();
        line++;
}

static bool
wait_polls(volatile uint8_t *reg, uint8_t mask, uint8_t value)
{
        for (uint32_t i = 0; i < WAIT_POLLS; i++) {
                if ((*reg & mask) == value) {
                        return true;
                }
        }

        return false;
}

/* Sends a command and keeps its answer. False when no answer comes or the CD block rejects it. */
static bool
command(uint16_t cr1, uint16_t cr2, uint16_t cr3, uint16_t cr4)
{
        CD_HIRQ = (uint16_t)~HIRQ_CMOK;
        CD_CR(0) = cr1;
        CD_CR(1) = cr2;
        CD_CR(2) = cr3;
        CD_CR(3) = cr4;

        uint32_t i;

        for (i = 0; i < WAIT_POLLS; i++) {
                if ((CD_HIRQ & HIRQ_CMOK) != 0) {
                        break;
                }
        }

        if (i == WAIT_POLLS) {
                return false;
        }

        for (unsigned n = 0; n < 4; n++) {
                answer[n] = CD_CR(n);
        }

        return (answer[0] >> 8) != 0xFF;
}

/* One line: 01's CR4, E1's result word, and the counters carried by CR4's low byte. */
static void
report(const char *step, unsigned frames)
{
        uint16_t cr4 = 0xFFFF;
        uint16_t result = 0xFFFF;

        if (command(0x0100, 0x0000, 0x0000, 0x0000)) {
                cr4 = answer[3];
        }

        if (command(0xE100, 0x0000, 0x0000, 0x0000)) {
                result = answer[1];
        }

        const uint8_t revision = cr4 & 0xFF;
        char text[64];

        snprintf(text, sizeof(text), "%-9s %04X %04X %4u task %X irq %X", step, cr4, result,
            frames, revision >> 4, revision & 0x0F);
        print(text);
}

/* E0 subcode 0 on partition 0, as the BIOS sends it, then its end, EFLS. */
static unsigned
authenticate(void)
{
        CD_HIRQ = (uint16_t)~HIRQ_EFLS;

        if (!command(0xE000, 0x0000, 0x0000, 0x0000)) {
                return 0xFFFF;
        }

        for (unsigned frames = 0; frames < AUTH_FRAMES; frames++) {
                if ((CD_HIRQ & HIRQ_EFLS) != 0) {
                        return frames;
                }

                wait_frames(1);
        }

        return 0xFFFF;
}

static bool
smpc_command(uint8_t smpc)
{
        if (!wait_polls(&SMPC_SF, 1, 0)) {
                return false;
        }

        SMPC_SF = 1;
        SMPC_COMREG = smpc;

        return wait_polls(&SMPC_SF, 1, 0);
}

/* The NUL CDBLOCK signature in CR1..CR4, which startup publishes. */
static bool
wait_signature(void)
{
        for (uint32_t i = 0; i < WAIT_POLLS; i++) {
                if ((CD_CR(0) == 0x0043) && (CD_CR(1) == 0x4442) && (CD_CR(2) == 0x4C4F) &&
                    (CD_CR(3) == 0x434B)) {
                        return true;
                }
        }

        return false;
}

/* Frames until 00's status is past the drive's startup. */
static unsigned
wait_drive_ready(void)
{
        for (unsigned frames = 0; frames < READY_FRAMES; frames++) {
                if (command(0x0000, 0x0000, 0x0000, 0x0000)) {
                        const uint8_t status = (answer[0] >> 8) & 0x1F;

                        if ((status != 0) && (status != 6) && (status != 7)) {
                                return frames;
                        }
                }

                wait_frames(1);
        }

        return 0xFFFF;
}

int
main(void)
{
        dbgio_init();
        dbgio_dev_default_init(DBGIO_DEV_VDP2_ASYNC);
        dbgio_dev_font_load();

        dbgio_puts("\x1b[2;2HDisc extension takeover\n");
        print("step      01CR4 E1   frames counters");

        report("boot", 0);

        command(0x0401, 0x0000, 0x0000, 0x0000);
        wait_frames(30);
        report("04 reset", 30);

        unsigned frames = authenticate();
        report("E0 #1", frames);

        frames = authenticate();
        report("E0 #2", frames);

        const bool off = smpc_command(SMPC_CDOFF);
        wait_frames(10);
        const bool on = smpc_command(SMPC_CDON) && wait_signature();
        report((off && on) ? "CDOFF/ON" : "CDOFF bad", 0);

        frames = wait_drive_ready();
        report("ready", frames);

        frames = authenticate();
        report("E0 #3", frames);

        print("done");

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
