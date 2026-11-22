/*
 * Streamed scroller with the card firmware off the disc, for a console with no MPEG card.
 *
 * The scroller of src/demo-discs/scroller, but the SH-1 code comes from this disc. Its header
 * names src/disc-extensions/firmware-boot-asm as its disc extension: after each accepted disc
 * authentication the CD block runs it, and it installs the card firmware where a card load would
 * and marks a card present. The cipher module is uploaded from this program, since the card ROM's
 * library is not there either.
 *
 * At boot, after libyaul's cd_block_init():
 * - E0 authenticates the disc again, so the extension runs for this disc whatever ran before it.
 *   E1 then gives the result, 4 for an accepted disc, and 01 the CD block revision the extension
 *   sets (B0) and the hardware flag, with bit 1 now set.
 * - 93 turns the firmware on, AD checks it is ours, and a load job installs the cipher module from
 *   the package built into this program.
 * - The description row then shows what each step gave: E1's result, 01's revision and hardware
 *   flag, and the module's handle, or the step that failed.
 *
 * The rest is the scroller's. A picture wider than the screen scrolls past. The strips come off the
 * disc, from STRIPS.BIN, each encrypted with XTEA under its strip number as its stream number. The
 * drive reads the file ahead into a partition of the CD block's buffer. Each strip then goes one of
 * two ways, as the toggle says:
 *
 * - Raw: its two sectors cross the data port as they are, so the picture shows as noise.
 * - SH-1: its sectors move inside the CD block into the SH-1's input partition, and a job for the
 *   uploaded cipher module decrypts them into the output partition, which crosses the data port.
 *   The SH-2 checks the strip against the checksum table built into the program.
 *
 * The status line counts the frames the scroll waited for a strip, the bad strips (a job or
 * transfer that failed, drawn red, or a checksum that differs), those the SH-1 decrypted and
 * checked, and all drawn.
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

/* How far past the screen's right edge strips are fetched, in pixels. */
#define LEAD 32

/* Steps a strip's fetch may take in one frame, so a frame always ends. */
#define STEPS_MOST 4

/* Frames without a strip drawn while the scroll waits before the stream counts as lost, 10 s. */
#define STALL_FRAMES 600

/* The partitions, each filled through the filter of the same number: the SH-1 job's input and
 * output, and the read-ahead's. Partition 0 is left to libyaul's own reads and to E0. */
#define INPUT 1
#define OUTPUT 2
#define READ 3

/* The read-ahead: a play of READ_PIECE sectors once the last has arrived and the partition holds
 * fewer than READ_LOW. */
#define READ_LOW 16
#define READ_PIECE 32

#define FILE_NAME "STRIPS.BIN"

/* The CD block's interrupt flags register, for E0's end, EFLS, which the client does not wait for. */
#define CD_HIRQ (*(volatile uint16_t *)0x25890008UL)
#define HIRQ_EFLS 0x0200

/* Frames E0 may take, past the firmware's own 20 s limit on the disc check. */
#define AUTH_FRAMES 1500

/* The CD block revision the disc extension sets, its byte +0x44. */
#define EXTENSION_REVISION 0xB0

/* The checksum table: the strip count, then each strip's checksum before encryption, big-endian
 * longwords. libyaul's bin2o aligns a built-in asset to 4 bytes. */
extern const uint32_t asset_strip_table[];

/* The cipher module's package. */
extern const uint8_t asset_cipher[];
extern const uint8_t asset_cipher_end[];

/* The description row, replaced by the bring-up's findings once they are in. */
static char description[40] = "Card firmware off disc, SH-1 XTEA";

static shell_game_t game = {
        .name = "Scroller, no card",
        .description = description,
        .toggle_labels = { "Raw", "SH-1 XTEA" }
};

/* A strip's sectors cross the data port into this by DMA, so it is aligned for that and for the
 * cache purge over it. The package is put from it too, padded to a whole sector. */
static uint8_t strip_buffer[STRIP_SECTORS * SH1_SECTOR_BYTES] __aligned(16);

static uint32_t file_fad;
static uint32_t strips;
static bool sh1_up;

/* The uploaded cipher module's handle. */
static uint8_t cipher;

/* Why the SH-1 takes no strips, for the status line. */
static const char *sh1_down = "No SH-1";

/* Set when taking a strip from the read-ahead's partition fails, or after STALL_FRAMES without a
 * strip. The stream stops there. */
static bool lost;

/* The file's sectors asked for by plays, taken from the partition's front, and arrived in it. */
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

/* E0 subcode 0 on partition 0, as the BIOS sends it, then its end, EFLS, a frame at a time so the
 * screen goes on. False when E0 is rejected or EFLS never comes. */
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

                shell_frame();
        }

        return false;
}

/*
 * The SH-1 up from the disc: E0, then E1's result and 01's revision and hardware flag, then 93,
 * AD, and the cipher module's load. The description row gets each value, and the step that failed.
 * True once the module is installed, its handle in cipher.
 */
static bool
sh1_bring_up(void)
{
        uint8_t result = 0xFF;
        uint8_t revision = 0xFF;
        uint8_t flag = 0xFF;
        const char *failed = NULL;

        shell_status("E0, disc check");
        shell_frame();

        if (!authenticate()) {
                failed = "E0";
        }

        if (sh1_command(0xE100, 0x0000, 0x0000, 0x0000, 0) &&
            ((sh1_answer[0] >> 8) != SH1_STATUS_REJECT)) {
                result = sh1_answer[1] & 0xFF;
        }

        if (sh1_command(0x0100, 0x0000, 0x0000, 0x0000, 0) &&
            ((sh1_answer[0] >> 8) != SH1_STATUS_REJECT)) {
                flag = sh1_answer[1] >> 8;
                revision = sh1_answer[3] & 0xFF;
        }

        if ((failed == NULL) && (revision != EXTENSION_REVISION)) {
                failed = "no ext";
        } else if ((failed == NULL) && ((flag & 0x02) == 0)) {
                failed = "no card bit";
        }

        uint16_t activations;
        sh1_job_t job;

        if ((failed == NULL) && !sh1_card_on()) {
                failed = "93";
        } else if ((failed == NULL) && !sh1_identity(&activations)) {
                failed = "AD";
        } else if ((failed == NULL) && !sh1_reset_selector()) {
                failed = "48";
        }

        if (failed == NULL) {
                const uint32_t bytes = (uint32_t)(asset_cipher_end - asset_cipher);
                const uint16_t sectors =
                    (uint16_t)((bytes + SH1_SECTOR_BYTES - 1) / SH1_SECTOR_BYTES);

                job.state = SH1_JOB_NONE;
                job.result = 0;

                if ((bytes == 0) || (bytes > sizeof(strip_buffer))) {
                        failed = "package";
                } else {
                        memset(strip_buffer, 0, sizeof(strip_buffer));
                        memcpy(strip_buffer, asset_cipher, bytes);

                        if (!sh1_module_load(INPUT, strip_buffer, sectors, ++fetch.id, &job) ||
                            (job.state != SH1_JOB_DONE)) {
                                failed = "load";
                        } else {
                                cipher = (uint8_t)job.result;
                        }
                }
        }

        if (failed == NULL) {
                (void)snprintf(description, sizeof(description),
                    "E1 %02X rev %02X hw %02X cipher %u", result, revision, flag, cipher);
        } else {
                (void)snprintf(description, sizeof(description),
                    "E1 %02X rev %02X hw %02X %s failed", result, revision, flag, failed);
                sh1_down = "SH-1 down";
        }

        return (failed == NULL);
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

/* The read-ahead's step: 51 counts the partition's sectors, and once the last piece has arrived
 * and the partition runs low, 10 plays the next, from the file's start after its end. */
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

/* SH-1: 66 moves the strip's sectors into the input partition, then AB submits the job to the
 * uploaded module. Its argument: CR3 the input partition and the output filter, CR4 the stream
 * number, the strip's number in the picture. With the SH-1 down the sectors are deleted and the
 * strip drawn red. */
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

        if (!sh1_submit(cipher, ++fetch.id, argument)) {
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

        /* The SH-1 up from the disc, with the cipher module installed. Then 48 connects each
         * filter to the partition of the same number, and 30 the drive's output to the
         * read-ahead's filter */
        sh1_up = sh1_bring_up();

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
