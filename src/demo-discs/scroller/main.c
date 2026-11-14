/*
 * Streamed scroller.
 *
 * A picture wider than the screen scrolls past. VDP2 scrolls the background by itself: each frame
 * the program only says which bitmap column the screen starts at. The bitmap is 512 columns wide
 * and the screen 320, so the columns off screen are where the next strips go, 8 pixels wide, each
 * drawn just before it scrolls into view. Past the bitmap's right end the screen wraps around to
 * its left end, which holds newer strips by then. After the picture's last strip comes its first
 * again, so the picture loops.
 *
 * The strips come off the disc, from STRIPS.BIN, each encrypted with XTEA under its strip number
 * as its stream number. The drive reads the file ahead into a partition of the CD block's buffer,
 * a piece at a time, from the file's start again after its end. Each strip then goes one of two
 * ways, as the toggle says:
 *
 * - Raw: its two sectors cross the data port as they are, so the picture shows as noise.
 * - SH-1: its sectors move inside the CD block into the SH-1's input partition, and a job for the
 *   card ROM library's cipher module decrypts them into the output partition, which crosses the
 *   data port. The SH-2 checks the strip against the checksum table built into the program.
 *
 * The same bytes cross the data port either way, only the decryption differs. A flip of the toggle
 * changes the strips fetched from then on, so the change sweeps in from the right edge.
 *
 * The status line counts the frames the scroll waited for a strip, the bad strips (a job or
 * transfer that failed, drawn red, or a checksum that differs), those the SH-1 decrypted and
 * checked, and all drawn. A failed take from the read-ahead's partition stops the stream, as does
 * STALL_FRAMES with no strip while the scroll waits, and the status line says so.
 *
 * At boot the disc checks that the card ROM's library has the cipher module and that its copy is
 * whole, the way a game checks for what it needs before it starts. Without it the SH-1 takes no
 * strips, and the status line says why: no cipher, or a bad one.
 */

#include <yaul.h>

#include <string.h>

#include <sh1-client.h>
#include <shell.h>

#define STRIP_WIDTH 8
#define STRIP_PIXELS (STRIP_WIDTH * SHELL_HEIGHT)
#define STRIP_BYTES (STRIP_PIXELS * 2)
#define STRIP_SECTORS 2

/* Pixels the picture moves each frame. */
#define SPEED 1

/* How far past the screen's right edge strips are fetched, in pixels. Further hides more of a
 * slow strip, nearer shows a flip of the toggle sooner. */
#define LEAD 32

/* Steps a strip's fetch may take in one frame, so a frame always ends. */
#define STEPS_MOST 4

/* Frames without a strip drawn while the scroll waits before the stream counts as lost, 10 s: a
 * play that never lands, or a job that never ends. */
#define STALL_FRAMES 600

/* The partitions, each filled through the filter of the same number: the SH-1 job's input and
 * output, and the read-ahead's. Partition 0 is left to libyaul's own reads. */
#define INPUT 1
#define OUTPUT 2
#define READ 3

/* The read-ahead: a play of READ_PIECE sectors once the last has arrived and the partition holds
 * fewer than READ_LOW, 8 strips, about a second of scrolling. */
#define READ_LOW 16
#define READ_PIECE 32

#define FILE_NAME "STRIPS.BIN"

/* The checksum table: the strip count, then each strip's checksum before encryption, big-endian
 * longwords. libyaul's bin2o aligns a built-in asset to 4 bytes. */
extern const uint32_t asset_strip_table[];

static const shell_game_t game = {
        .name = "Streamed scroller",
        .description = "XTEA strips off disc, SH-1 decrypts",
        .toggle_labels = { "Raw", "SH-1 XTEA" }
};

/* A strip's sectors cross the data port into this by DMA, so it is aligned for that and for the
 * cache purge over it. */
static uint8_t strip_buffer[STRIP_SECTORS * SH1_SECTOR_BYTES] __aligned(16);

static uint32_t file_fad;
static uint32_t strips;
static bool sh1_up;

/* Why the SH-1 takes no strips, for the status line: no card firmware, or a card ROM whose library
 * lacks the cipher module or holds a damaged copy. */
static const char *sh1_down = "No SH-1";

/* Set when taking a strip from the read-ahead's partition fails (63 or the 06 that ends it, 66,
 * 62), which leaves its sectors in doubt, so strips would no longer be where they are counted, or
 * after STALL_FRAMES without a strip. The stream stops there. */
static bool lost;

/* The file's sectors asked for by plays, taken from the partition's front, and arrived in it.
 * They count on past the file's end: a count's sector in the file is the count modulo the file's
 * sectors. */
static struct {
        uint32_t requested;
        uint32_t taken;
        uint32_t arrived;
} reads;

/* The strip on its way: none, its job on the SH-1, or its sectors crossing the data port. */
typedef enum {
        FETCH_NONE,
        FETCH_DECRYPTING,
        FETCH_CROSSING
} fetch_state_t;

static struct {
        fetch_state_t state;
        bool sh1;
        /* Each job's ID, new for every job so a stale answer cannot pass for its own */
        uint16_t id;
} fetch;

/* Strips drawn, counted on past the picture's end, so the next to draw is strip drawn. */
static uint32_t drawn;

static struct {
        uint32_t sh1;
        uint32_t bad;
        uint32_t late;
} counts;

/* The file's place on the disc, from the disc's ISO 9660 file table, and the table's strip count.
 * True when the file holds every strip. */
static bool
file_find(void)
{
        static cdfs_filelist_entry_t entries[16];
        static cdfs_filelist_t filelist;

        cdfs_config_default_set();
        cdfs_filelist_init(&filelist, entries, 16);
        cdfs_filelist_root_read(&filelist);

        strips = asset_strip_table[0];

        for (uint32_t i = 0; i < filelist.entries_count; i++) {
                if ((entries[i].type == CDFS_ENTRY_TYPE_FILE) &&
                    (strcmp(entries[i].name, FILE_NAME) == 0) &&
                    (entries[i].size == strips * STRIP_SECTORS * SH1_SECTOR_BYTES)) {
                        file_fad = entries[i].starting_fad;
                }
        }

        return (file_fad != 0) && (strips != 0);
}

static uint32_t
checksum(const uint8_t *data)
{
        const uint32_t *const words = (const uint32_t *)data;
        uint32_t sum = 0;

        for (uint32_t i = 0; i < STRIP_BYTES / 4; i++) {
                sum = sum * 31 + words[i];
        }

        return sum;
}

/* The next strip into its columns of the bitmap: data's pixels, or red with no data. */
static void
strip_draw(const uint8_t *data)
{
        uint16_t *const column = shell_bitmap() + (drawn * STRIP_WIDTH) % SHELL_BITMAP_WIDTH;
        const uint16_t *const pixels = (const uint16_t *)data;
        const uint16_t red = RGB1555(1, 31, 0, 0).raw;

        for (uint32_t y = 0; y < SHELL_HEIGHT; y++) {
                for (uint32_t x = 0; x < STRIP_WIDTH; x++) {
                        column[y * SHELL_BITMAP_WIDTH + x] =
                            (data != NULL) ? pixels[y * STRIP_WIDTH + x] : red;
                }
        }

        drawn++;
}

static void
strip_failed(void)
{
        counts.bad++;
        strip_draw(NULL);
}

/*
 * The read-ahead's step: 51 counts the partition's sectors, and once the last piece has arrived
 * and the partition runs low, 10 plays the next, from the file's start after its end. The drive
 * files the sectors on its own while the master goes on. No command may go to the CD block while
 * a transfer is open, so this waits for none.
 */
static void
read_ahead(void)
{
        const uint32_t file_sectors = strips * STRIP_SECTORS;
        uint16_t held;

        if (sh1_transfer_open() || !sh1_sectors(READ, &held)) {
                return;
        }

        reads.arrived = reads.taken + held;

        if ((reads.arrived >= reads.requested) && (held < READ_LOW)) {
                const uint32_t at = reads.requested % file_sectors;
                uint32_t piece = file_sectors - at;

                if (piece > READ_PIECE) {
                        piece = READ_PIECE;
                }

                if (sh1_play(file_fad + at, (uint16_t)piece)) {
                        reads.requested += piece;
                }
        }
}

/* Raw: 63 gets the strip's sectors from the read-ahead's front by DMA and deletes them once the
 * transfer ends. */
static bool
raw_start(void)
{
        if (!sh1_get_start(READ, 0, STRIP_SECTORS, strip_buffer, true)) {
                lost = true;
                return false;
        }

        reads.taken += STRIP_SECTORS;
        fetch.state = FETCH_CROSSING;

        return true;
}

/* SH-1: 66 moves the strip's sectors into the input partition, then AB submits the job. Its
 * argument: CR3 the input partition and the output filter, CR4 the stream number, the strip's
 * number in the picture. With the SH-1 down the sectors are deleted and the strip drawn red. */
static bool
decrypt_start(void)
{
        if (!sh1_up) {
                if (!sh1_delete(READ, 0, STRIP_SECTORS)) {
                        lost = true;
                        return false;
                }

                reads.taken += STRIP_SECTORS;
                strip_failed();

                return true;
        }

        if (!sh1_move(READ, 0, STRIP_SECTORS, INPUT)) {
                lost = true;
                return false;
        }

        reads.taken += STRIP_SECTORS;

        const uint32_t argument = ((uint32_t)INPUT << 24) | ((uint32_t)OUTPUT << 16) |
            (drawn % strips);

        if (!sh1_submit(SH1_LIBRARY_CIPHER, ++fetch.id, argument)) {
                (void)sh1_delete(INPUT, 0, 0xFFFF);
                strip_failed();
                return true;
        }

        fetch.state = FETCH_DECRYPTING;

        return true;
}

/* AC asks for the job's record. Once done, 63 gets its output by DMA and deletes it once the
 * transfer ends. */
static bool
decrypt_poll(void)
{
        sh1_job_t job;

        if (!sh1_status(&job)) {
                /* No answer, so the job may still run and file sectors: the SH-1 gets no more */
                sh1_up = false;
                fetch.state = FETCH_NONE;
                strip_failed();
                return true;
        }

        const bool ours = (job.id == fetch.id);

        if (ours && ((job.state == SH1_JOB_SUBMITTED) || (job.state == SH1_JOB_RUNNING))) {
                return false;
        }

        if (ours && (job.state == SH1_JOB_DONE) && (job.result == STRIP_SECTORS) &&
            sh1_get_start(OUTPUT, 0, STRIP_SECTORS, strip_buffer, true)) {
                fetch.state = FETCH_CROSSING;
                return true;
        }

        (void)sh1_delete(INPUT, 0, 0xFFFF);
        (void)sh1_delete(OUTPUT, 0, 0xFFFF);
        fetch.state = FETCH_NONE;
        strip_failed();

        return true;
}

/* 06 ends the transfer once its DMA has, then the strip is drawn: checked first when the SH-1
 * decrypted it, its input partition emptied. */
static bool
crossing_end(void)
{
        if (sh1_transfer_busy()) {
                return false;
        }

        bool ended = sh1_transfer_end();

        if (fetch.sh1) {
                ended = sh1_delete(INPUT, 0, 0xFFFF) && ended;
        }

        fetch.state = FETCH_NONE;

        if (!ended) {
                if (!fetch.sh1) {
                        lost = true;
                        return false;
                }

                (void)sh1_delete(OUTPUT, 0, 0xFFFF);
                strip_failed();
                return true;
        }

        if (fetch.sh1) {
                if (checksum(strip_buffer) == asset_strip_table[1 + drawn % strips]) {
                        counts.sh1++;
                } else {
                        counts.bad++;
                }
        }

        strip_draw(strip_buffer);

        return true;
}

/* Moves the strip on its way along, or starts the next once it is within LEAD of the screen's
 * right edge and its sectors have arrived. True when it can go on at once. */
static bool
fetch_step(uint32_t scroll)
{
        switch (fetch.state) {
        case FETCH_DECRYPTING:
                return decrypt_poll();
        case FETCH_CROSSING:
                return crossing_end();
        default:
                break;
        }

        if (lost || (drawn * STRIP_WIDTH >= scroll + SHELL_WIDTH + LEAD) ||
            (reads.arrived < reads.taken + STRIP_SECTORS)) {
                return false;
        }

        fetch.sh1 = shell_toggle();

        return fetch.sh1 ? decrypt_start() : raw_start();
}

int
main(void)
{
        shell_init(&game);

        /* libyaul's CD reads first: its cd_block_init() restarts the CD block, which would turn
         * the card firmware off again */
        cd_block_init();

        const bool found = file_find();

        /* The SH-1 up: E0 loads the card firmware from the MPEG card, 93 turns it on, AD checks
         * it is ours. Then 48 connects each filter to the partition of the same number, and 30
         * the drive's output to the read-ahead's filter */
        uint16_t activations;

        sh1_up = sh1_card_load() && sh1_card_on() && sh1_identity(&activations);

        /* The boot check: the cipher module in the card ROM's library, and its copy whole. A card
         * with an older library says so here, where otherwise each strip's job would fail one by
         * one. The check loads the module, so the first strip finds it installed. A firmware
         * without the check goes on unchecked, and its jobs load the module themselves. */
        if (sh1_up) {
                sh1_job_t job;

                switch (sh1_library_check(SH1_LIBRARY_CIPHER, ++fetch.id, &job)) {
                case SH1_LIBRARY_MISSING:
                        sh1_up = false;
                        sh1_down = "No cipher";
                        break;
                case SH1_LIBRARY_BAD:
                        sh1_up = false;
                        sh1_down = "Bad cipher";
                        break;
                default:
                        break;
                }
        }

        if (!found || !sh1_reset_selector() || !sh1_connect(READ)) {
                shell_status("%s", found ? "CD block not answering" : FILE_NAME " missing or bad");

                while (true) {
                        shell_frame();
                }
        }

        /* The picture's column at the screen's left edge, and whether it has moved yet */
        uint32_t scroll = 0;
        bool started = false;

        /* Frames in a row with the scroll waiting and no strip drawn, and the strips drawn then */
        uint32_t stalled = 0;
        uint32_t drawn_before = 0;

        while (true) {
                for (uint32_t i = 0; (i < STEPS_MOST) && fetch_step(scroll); i++) {
                }

                read_ahead();

                /* The scroll moves on once every column the next screen shows is drawn */
                if (drawn * STRIP_WIDTH >= scroll + SPEED + SHELL_WIDTH) {
                        scroll += SPEED;
                        started = true;
                        stalled = 0;
                } else {
                        if (started) {
                                counts.late++;
                        }

                        stalled = (drawn != drawn_before) ? 0 : stalled + 1;

                        if (stalled > STALL_FRAMES) {
                                lost = true;
                        }
                }

                drawn_before = drawn;

                shell_scroll(scroll);

                /* The waits and failures first, so a long run's growing counts cut only the
                 * strips drawn */
                if (lost) {
                        shell_status("Stream LOST after %lu strips", (unsigned long)drawn);
                } else if (sh1_up) {
                        shell_status("Late %lu, bad %lu, SH-1 %lu, drawn %lu",
                            (unsigned long)counts.late, (unsigned long)counts.bad,
                            (unsigned long)counts.sh1, (unsigned long)drawn);
                } else {
                        shell_status("%s. Late %lu, bad %lu, drawn %lu", sh1_down,
                            (unsigned long)counts.late, (unsigned long)counts.bad,
                            (unsigned long)drawn);
                }

                shell_frame();
        }

        return 0;
}
