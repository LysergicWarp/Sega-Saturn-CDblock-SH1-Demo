/*
 * Card ROM module library.
 *
 * The CD block's SH-1 runs modules, small programs the card firmware installs in a 16 KB slot of
 * the SH-1's memory. The first disc uploads its module: the SH-2 sends the package through the CD
 * block's data port into a partition of the CD buffer, and a load job checks it and installs it.
 *
 * The card ROM can carry modules too, in a library beside the firmware. Each one sits on a fixed
 * handle, from 251 to 255. This disc uses three, named by the client's SH1_LIBRARY_DECOMPRESS,
 * SH1_LIBRARY_TRANSFORM and SH1_LIBRARY_CIPHER the way a game names a library's functions. A job
 * sent to one of those needs no upload and no load: when the slot holds another module, the
 * firmware copies the right one out of the card ROM first, then runs the job.
 *
 * The screen runs three jobs both ways, a PRS decode of this file (main.c, compressed at build
 * time), a transform of a sector of vertices and an XTEA decryption of this file (encrypted at
 * build time), every output checked against the SH-2's own:
 *
 * 1. Uploaded: each module's upload and load, then its job.
 * 2. From the card ROM: each module's first job, which loads it from the card ROM, then the same
 *    job again with the module already in the slot.
 * 3. Alternating: decodes and transforms in turn, so each job swaps the module in the slot.
 *
 * Times come from the SH-2's free-running timer, in emulated milliseconds. They cover the commands
 * and the SH-1's work, not the input's put and the output's get, which are the same both ways.
 */

#include <yaul.h>

#include <string.h>

#include <sh1-client.h>

/* The partitions, each filled through the filter of the same number. */
#define INPUT 0
#define OUTPUT 1

/* The buffer's slots. Those the input leaves free bound the output. */
#define SLOTS 200

#define PUT_SECTORS 10
#define GOT_SECTORS 10
#define VECTORS 252

/* Jobs in the alternating run, half to each module. */
#define ALTERNATING 16

/* The cipher's stream number, which picks its key stream, the one this file was encrypted with
 * (CIPHER_STREAM in build.py). */
#define CIPHER_STREAM 291

/* The modules' packages, from src/modules-asm, the stream and the encrypted file, all built into
 * the program. */
extern const uint8_t asset_decompress[];
extern const uint8_t asset_decompress_end[];
extern const uint8_t asset_transform[];
extern const uint8_t asset_transform_end[];
extern const uint8_t asset_cipher[];
extern const uint8_t asset_cipher_end[];
extern const uint8_t asset_stream[];
extern const uint8_t asset_stream_end[];
extern const uint8_t asset_original[];
extern const uint8_t asset_original_end[];
extern const uint8_t asset_encrypted[];
extern const uint8_t asset_encrypted_end[];

/* Sectors cross the data port by DMA, so the buffers are aligned for it and the cache purge. */
static uint8_t put_buffer[PUT_SECTORS * SH1_SECTOR_BYTES] __aligned(16);
static uint8_t got[GOT_SECTORS * SH1_SECTOR_BYTES] __aligned(16);
static int16_t vertices[SH1_SECTOR_BYTES / 2];
static int16_t expected[SH1_SECTOR_BYTES / 2];

/* The stream's sectors once staged. */
static uint16_t stream_sectors;

/* Each job's ID, new for every job so a stale answer cannot pass for its own. */
static uint16_t job_id;

/* Steps that failed, from the bring-up to the last check. */
static uint32_t failures;

/* A rotation by 30 degrees about z in Q12 with a translation, rows of (m0, m1, m2, t). */
static const int16_t matrix[3][4] = {
        { 3547, -2048, 0, 100 },
        { 2048, 3547, 0, -200 },
        { 0, 0, 4096, 300 }
};

/*
 * Time. The SH-2's free-running timer (FRT) counts the CPU clock divided by 128, 26.8741 MHz at
 * this resolution on an NTSC console, so 209.95 counts a millisecond. A PAL console's clock is 0.7%
 * slower, so its times read that much short. The timer is 16 bits wide and comes round every
 * 312 ms, so its overflow interrupt counts the rounds.
 */
static volatile uint32_t rounds;

static void
frt_overflow(void)
{
        rounds++;
}

static uint32_t
ticks(void)
{
        uint32_t high;
        uint16_t low;

        /* Read again if a round ends between the two reads */
        do {
                high = rounds;
                low = cpu_frt_count_get();
        } while (high != rounds);

        return (high << 16) | low;
}

/* Hundredths of a millisecond in ticks */
static uint32_t
hundredths(uint32_t ticks)
{
        return (uint32_t)((uint64_t)ticks * 10000 / 20995);
}

/* Ends a frame: the text written so far goes to VDP2, the video chip that draws it, at the next
 * vertical blank. The flush queues its transfer, then the call that marks it done. A vertical
 * blank between the two would send the text and never mark it, and libyaul drops every flush
 * after an unmarked one, so interrupts are masked across it. */
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

/* Copies the bytes from start to end into the put buffer, zeros after them to whole sectors.
 * Returns the sectors, or 0 for more than the buffer holds. */
static uint16_t
stage(const uint8_t *start, const uint8_t *end)
{
        const uint32_t bytes = (uint32_t)(end - start);
        const uint32_t sectors = (bytes + SH1_SECTOR_BYTES - 1) / SH1_SECTOR_BYTES;

        if (sectors > PUT_SECTORS) {
                return 0;
        }

        memcpy(put_buffer, start, bytes);
        memset(put_buffer + bytes, 0, sectors * SH1_SECTOR_BYTES - bytes);

        return (uint16_t)sectors;
}

/*
 * The sector of vertices and its transform: the matrix padded to 32 bytes, then vectors of
 * (x, y, z, w). Each row of the transform gives the low 16 bits of ((m0 x + m1 y + m2 z) >> 12)
 * + t from a 32-bit sum, and w is copied. The SH-2 computes it here to check the SH-1's.
 */
static void
make_vertices(void)
{
        for (unsigned i = 0; i < 16; i++) {
                vertices[i] = (i < 12) ? matrix[i / 4][i % 4] : 0;
                expected[i] = vertices[i];
        }

        for (int i = 0; i < VECTORS; i++) {
                int16_t *const x = &vertices[16 + 4 * i];
                int16_t *const out = &expected[16 + 4 * i];

                x[0] = (int16_t)((i * 37) % 4096 - 2048);
                x[1] = (int16_t)((i * 91) % 4096 - 2048);
                x[2] = (int16_t)((i * 13) % 2048 - 1024);
                x[3] = (int16_t)i;

                for (unsigned r = 0; r < 3; r++) {
                        const int16_t *const m = matrix[r];
                        const uint32_t sum = (uint32_t)(m[0] * x[0] + m[1] * x[1] + m[2] * x[2]);

                        out[r] = (int16_t)(uint16_t)((sum >> 12) + (uint32_t)m[3]);
                }

                out[3] = x[3];
        }
}

/* Uploads a module's package and loads it: its handle, or 0 when that fails. *spent is the time. */
static uint8_t
upload(const uint8_t *start, const uint8_t *end, uint32_t *spent)
{
        const uint16_t sectors = stage(start, end);
        sh1_job_t job;

        const uint32_t began = ticks();
        const bool loaded = (sectors != 0) &&
            sh1_module_load(INPUT, put_buffer, sectors, ++job_id, &job);
        *spent = ticks() - began;

        if (!loaded) {
                failures++;
                return 0;
        }

        return (uint8_t)job.result;
}

/*
 * The jobs. Each puts its input into INPUT, untimed, then submits the job and waits for its end,
 * timed, then gets the output from OUTPUT and checks it, and empties both partitions. A module's
 * job argument: CR3 the input partition and the output filter, CR4 the most output sectors, or
 * the cipher's stream number. Each returns the time, and counts a failure when anything differs.
 */
static uint32_t
run(uint8_t handle, uint16_t cr4, sh1_job_t *job)
{
        const uint32_t argument = ((uint32_t)INPUT << 24) | ((uint32_t)OUTPUT << 16) | cr4;
        const uint16_t id = ++job_id;

        job->state = SH1_JOB_NONE;

        const uint32_t began = ticks();
        const bool ran = sh1_submit(handle, id, argument) && sh1_wait(id, job) &&
            (job->state == SH1_JOB_DONE);
        const uint32_t spent = ticks() - began;

        if (!ran) {
                failures++;
        }

        return spent;
}

static uint32_t
decode(uint8_t handle)
{
        const uint32_t original = (uint32_t)(asset_original_end - asset_original);
        sh1_job_t job;

        if (stage(asset_stream, asset_stream_end) != stream_sectors ||
            !sh1_put(INPUT, stream_sectors, put_buffer)) {
                failures++;
        }

        const uint32_t spent = run(handle, SLOTS - stream_sectors, &job);
        const uint32_t filed = (job.result + SH1_SECTOR_BYTES - 1) / SH1_SECTOR_BYTES;

        if (job.result != original || filed > GOT_SECTORS ||
            !sh1_get(OUTPUT, 0, (uint16_t)filed, got) ||
            memcmp(got, asset_original, original) != 0) {
                failures++;
        }

        if (!sh1_delete(INPUT, 0, 0xFFFF) || !sh1_delete(OUTPUT, 0, 0xFFFF)) {
                failures++;
        }

        return spent;
}

static uint32_t
transform(uint8_t handle)
{
        sh1_job_t job;

        memcpy(put_buffer, vertices, SH1_SECTOR_BYTES);

        if (!sh1_put(INPUT, 1, put_buffer)) {
                failures++;
        }

        const uint32_t spent = run(handle, 1, &job);

        if (job.result != 1 || !sh1_get(OUTPUT, 0, 1, got) ||
            memcmp(got, expected, SH1_SECTOR_BYTES) != 0) {
                failures++;
        }

        if (!sh1_delete(INPUT, 0, 0xFFFF) || !sh1_delete(OUTPUT, 0, 0xFFFF)) {
                failures++;
        }

        return spent;
}

/* The decryption: the encrypted file in, its sectors decrypted out, each one filed, and the file
 * then equal to the original, the padding after it zeros. */
static uint32_t
decrypt(uint8_t handle)
{
        const uint32_t original = (uint32_t)(asset_original_end - asset_original);
        const uint16_t sectors = stage(asset_encrypted, asset_encrypted_end);
        sh1_job_t job;

        if (sectors == 0 || !sh1_put(INPUT, sectors, put_buffer)) {
                failures++;
        }

        const uint32_t spent = run(handle, CIPHER_STREAM, &job);
        bool equal = (sectors <= GOT_SECTORS) && (job.result == sectors) &&
            sh1_get(OUTPUT, 0, sectors, got) && (memcmp(got, asset_original, original) == 0);

        for (uint32_t i = original; equal && (i < sectors * SH1_SECTOR_BYTES); i++) {
                equal = (got[i] == 0);
        }

        if (!equal) {
                failures++;
        }

        if (!sh1_delete(INPUT, 0, 0xFFFF) || !sh1_delete(OUTPUT, 0, 0xFFFF)) {
                failures++;
        }

        return spent;
}

/*
 * The check a game makes at boot: each library module it needs, in the card ROM and whole. The
 * firmware loads the module from the card ROM and checks its CRC on the way, which a job's own
 * load skips. A card with an older library then says so here, by the module's handle, before any
 * job fails. The card firmware in C has no check, and its line says unchecked.
 *
 * False when a module is missing or damaged. The card ROM's jobs are then not run: a job on a
 * damaged module would load it without the CRC and run whatever the copy now holds.
 */
static bool
library_check(void)
{
        static const uint8_t handles[] = {
                SH1_LIBRARY_DECOMPRESS, SH1_LIBRARY_TRANSFORM, SH1_LIBRARY_CIPHER
        };
        const char *found = "checked";

        for (unsigned i = 0; i < sizeof(handles) / sizeof(handles[0]); i++) {
                sh1_job_t job;

                switch (sh1_library_check(handles[i], ++job_id, &job)) {
                case SH1_LIBRARY_OK:
                        continue;
                case SH1_LIBRARY_UNCHECKED:
                        found = "unchecked";
                        continue;
                case SH1_LIBRARY_MISSING:
                        dbgio_printf("\x1b[4;2HCard ROM lacks module %u\n", handles[i]);
                        break;
                default:
                        dbgio_printf("\x1b[4;2HCard ROM's module %u is damaged\n", handles[i]);
                        break;
                }

                failures++;
                frame_end();

                return false;
        }

        dbgio_printf("\x1b[4;2HCard ROM library %s\n", found);
        frame_end();

        return true;
}

/* A row: a module, its handle and two times in milliseconds with two decimals */
static void
row(unsigned line, const char *name, uint8_t handle, uint32_t first, uint32_t second)
{
        const uint32_t a = hundredths(first);
        const uint32_t b = hundredths(second);

        dbgio_printf("\x1b[%u;2H  %-11s %3u  %4lu.%02lu  %4lu.%02lu\n", line, name, handle,
            (unsigned long)(a / 100), (unsigned long)(a % 100), (unsigned long)(b / 100),
            (unsigned long)(b % 100));
        frame_end();
}

int
main(void)
{
        dbgio_init();
        dbgio_dev_default_init(DBGIO_DEV_VDP2_ASYNC);
        dbgio_dev_font_load();

        dbgio_puts("\x1b[2;2HCard ROM module library\n");
        dbgio_puts("\x1b[26;2HTimes in emulated milliseconds.\n");
        frame_end();

        cpu_frt_init(CPU_FRT_CLOCK_DIV_128);
        cpu_frt_ovi_set(frt_overflow);
        cpu_frt_interrupt_priority_set(8);

        /*
         * The SH-1 up: E0 loads the card firmware from the MPEG card, 93 turns it on, AD checks it
         * is ours, and 48 connects each filter to the partition of the same number.
         */
        uint16_t activations = 0;
        const bool up = sh1_card_load() && sh1_card_on() && sh1_identity(&activations) &&
            sh1_reset_selector();

        dbgio_printf("\x1b[3;2HSH-1 firmware %s\n", up ? "up" : "not loaded, no card");
        frame_end();

        const bool library = up && library_check();

        stream_sectors = stage(asset_stream, asset_stream_end);

        if (!up || stream_sectors == 0) {
                while (true) {
                }
        }

        make_vertices();

        /* 1. Uploaded: the package through the data port, a load job, then the module's job */
        dbgio_puts("\x1b[5;2HUploaded over the data port\n");
        dbgio_puts("\x1b[6;2H              handle   load    job\n");
        frame_end();

        uint32_t load;
        uint8_t handle = upload(asset_decompress, asset_decompress_end, &load);
        row(7, "Decompress", handle, load, decode(handle));

        handle = upload(asset_transform, asset_transform_end, &load);
        row(8, "Transform", handle, load, transform(handle));

        handle = upload(asset_cipher, asset_cipher_end, &load);
        row(9, "XTEA cipher", handle, load, decrypt(handle));

        /* The library failed its check: its jobs stay unrun, and the failure is already counted */
        if (!library) {
                dbgio_puts("\x1b[11;2HCard ROM jobs not run\n");
                dbgio_puts("\x1b[20;2HSome outputs FAILED\n");
                frame_end();

                while (true) {
                }
        }

        /* 2. From the card ROM: the first job loads the module from it, the second finds it */
        dbgio_puts("\x1b[11;2HFrom the card ROM, fixed handles\n");
        dbgio_puts("\x1b[12;2H              handle  first  again\n");
        frame_end();

        const uint32_t decode_first = decode(SH1_LIBRARY_DECOMPRESS);
        row(13, "Decompress", SH1_LIBRARY_DECOMPRESS, decode_first,
            decode(SH1_LIBRARY_DECOMPRESS));

        const uint32_t transform_first = transform(SH1_LIBRARY_TRANSFORM);
        row(14, "Transform", SH1_LIBRARY_TRANSFORM, transform_first,
            transform(SH1_LIBRARY_TRANSFORM));

        const uint32_t decrypt_first = decrypt(SH1_LIBRARY_CIPHER);
        row(15, "XTEA cipher", SH1_LIBRARY_CIPHER, decrypt_first, decrypt(SH1_LIBRARY_CIPHER));

        /* 3. Alternating: every job finds the other module in the slot and swaps it */
        dbgio_printf("\x1b[17;2HAlternating %u and %u, %u jobs\n", SH1_LIBRARY_DECOMPRESS,
            SH1_LIBRARY_TRANSFORM, ALTERNATING);
        frame_end();

        uint32_t total = 0;

        for (unsigned i = 0; i < ALTERNATING; i++) {
                total += (i % 2 == 0) ? decode(SH1_LIBRARY_DECOMPRESS) :
                    transform(SH1_LIBRARY_TRANSFORM);
        }

        const uint32_t each = hundredths(total / ALTERNATING);

        dbgio_printf("\x1b[18;2H  %lu.%02lu a job, a swap in each\n",
            (unsigned long)(each / 100), (unsigned long)(each % 100));

        dbgio_printf("\x1b[20;2H%s\n",
            (failures == 0) ? "Every output verified" : "Some outputs FAILED");
        frame_end();

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
