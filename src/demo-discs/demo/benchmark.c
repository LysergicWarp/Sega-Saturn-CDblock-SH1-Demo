#include <yaul.h>

#include <string.h>

#include <dispatch.h>
#include <sh1-client.h>

#include "benchmark.h"

#define SECTOR_BYTES 2048

/*
 * The disc file and its chunk table, made by tools/bench.py when the disc is built. Each chunk's
 * stream starts at a sector of its own, so a job is whole sectors. The table: the chunk count,
 * then per chunk its first sector, its stream's bytes, its output's bytes and the output's
 * checksum, all big-endian longwords, the SH-2's own byte order.
 */
#define FILE_NAME "BENCH.PRS"
#define CHUNKS_MOST 256
#define CHUNK_OUTPUT_MOST (16 * SECTOR_BYTES)
#define CHUNK_INPUT_MOST (8 * SECTOR_BYTES)

extern const uint8_t asset_bench_table[];

typedef struct {
        uint32_t first_sector;
        uint32_t stream_bytes;
        uint32_t output_bytes;
        uint32_t checksum;
} chunk_t;

static chunk_t chunks[CHUNKS_MOST];
static uint32_t chunk_count;
static uint32_t file_fad;
static uint32_t file_sectors;

/*
 * The read-ahead. The drive plays the file in pieces into one partition of the CD block's buffer,
 * and jobs take chunks from its front. A piece starts only when the partition runs low, so the
 * buffer (200 sectors) always keeps room for the SH-1's output.
 */
#define READ_PARTITION DISPATCH_FIRST_FREE
#define READ_LOW 80
#define READ_PIECE 96
#define BUFFER_SECTORS 200

/* The worst case: just under READ_LOW held when a piece starts, the whole piece, and one SH-1 job's
 * input and output */
_Static_assert((READ_LOW - 1) + READ_PIECE + (CHUNK_INPUT_MOST / SECTOR_BYTES) +
        (CHUNK_OUTPUT_MOST / SECTOR_BYTES) <= BUFFER_SECTORS,
    "the read-ahead leaves the SH-1's output its slots");

/*
 * The transform: BATCHES batches of BATCH_VERTICES, done SLICE_VERTICES at a time between polls.
 * BATCHES is sized so the master's transform takes a time of the same order as the SH-1's decode of
 * the whole file (about 13 s emulated, the transform beside it some seconds more), where handing
 * the decode to the SH-1 pays off most.
 */
#define BATCHES 1200
#define BATCH_VERTICES 2048
#define SLICE_VERTICES 256

/* The slave takes the transform GROUP_BATCHES batches at a time, as one job. */
#define GROUP_BATCHES 4

/* With the SH-1 decoding too, the slave decodes only once this many chunks wait in the buffer,
 * and transforms otherwise: the SH-1 takes the decoding the slave would leave the transform for. */
#define SLAVE_BACKLOG 3

/* The CD block is asked about reads and the SH-1's jobs once every POLL_SLICES slices, as each
 * command takes the master a while. */
#define POLL_SLICES 4

/* Frames without a chunk or batch finished before a run gives up: 10 s at 60 frames a second, 12 s
 * on a 50 Hz (PAL) console. */
#define STALL_FRAMES 600

static fix16_vec3_t mesh[BATCH_VERTICES];
static fix16_vec3_t transformed[BATCH_VERTICES];

/* The slave's transform job: its batches, and its own output, so the two SH-2s never write the
 * same memory. */
static struct {
        uint32_t first;
        uint32_t count;
} slave_batches;

static fix16_vec3_t slave_transformed[BATCH_VERTICES];

/*
 * The spare work: what an SH-2 does with nothing else left, counted but never waited for. A step
 * moves PARTICLES particles by their velocity under gravity, bouncing them off a floor, the master
 * SLICE_PARTICLES at a time, the slave FILL_STEPS steps a job. Each SH-2 has its own particles.
 */
#define PARTICLES 2048
#define SLICE_PARTICLES 256
#define FILL_STEPS 2

typedef struct {
        fix16_vec3_t position;
        fix16_vec3_t velocity;
} particle_t;

static particle_t master_particles[PARTICLES];
static particle_t slave_particles[PARTICLES];

/* What a helper's job is. */
typedef enum {
        JOB_DECODE,
        JOB_TRANSFORM,
        JOB_SPARE
} job_kind_t;

/* Each helping processor's job and buffers. The SH-1's input never comes to SH-2 memory. */
typedef struct {
        dispatch_processor_t processor;
        dispatch_job_t job;
        job_kind_t kind;
        /* The chunk a decode takes, or the batches or steps of the other jobs */
        uint32_t chunk;
        bool busy;
        uint8_t *input;
        uint8_t *output;
} worker_t;

static uint8_t master_input[CHUNK_INPUT_MOST] __aligned(16);
static uint8_t master_output[CHUNK_OUTPUT_MOST] __aligned(16);
static uint8_t slave_input[CHUNK_INPUT_MOST] __aligned(16);
static uint8_t slave_output[CHUNK_OUTPUT_MOST] __aligned(16);
static uint8_t sh1_output[CHUNK_OUTPUT_MOST] __aligned(16);

static worker_t workers[DISPATCH_PROCESSORS] = {
        {.processor = DISPATCH_MASTER, .input = master_input, .output = master_output},
        {.processor = DISPATCH_SLAVE, .input = slave_input, .output = slave_output},
        {.processor = DISPATCH_SH1, .input = NULL, .output = sh1_output}
};

/* The runs. A run with the slave also has it share the transform with the master. */
static const struct {
        const char *name;
        /* Which processors decode, by bit (1 << processor) */
        uint32_t decoders;
} runs[BENCHMARK_RUNS] = {
        {"Master", 1u << DISPATCH_MASTER},
        {"Master + slave", 1u << DISPATCH_SLAVE},
        {"Master + SH-1", 1u << DISPATCH_SH1},
        {"Master + slave + SH-1", (1u << DISPATCH_SLAVE) | (1u << DISPATCH_SH1)}
};

/* Video frames since the start, counted at each vertical blank's start. */
static volatile uint32_t frames;

/* Frames a second: 60 on an NTSC console, 50 on a PAL one. */
static uint32_t frame_rate = 60;

static void
vblank_in(void *work __unused)
{
        frames++;
}

static uint32_t
sectors_of(uint32_t bytes)
{
        return (bytes + SECTOR_BYTES - 1) / SECTOR_BYTES;
}

static uint32_t
checksum(const uint8_t *data, uint32_t bytes)
{
        const uint32_t *const words = (const uint32_t *)data;
        uint32_t sum = 0;

        for (uint32_t i = 0; i < bytes / 4; i++) {
                sum = sum * 31 + words[i];
        }

        return sum;
}

bool
benchmark_init(void)
{
        static cdfs_filelist_entry_t entries[16];
        static cdfs_filelist_t filelist;

        /* The file's place on the disc, from the disc's ISO 9660 file table */
        cdfs_config_default_set();
        cdfs_filelist_init(&filelist, entries, 16);
        cdfs_filelist_root_read(&filelist);

        file_fad = 0;

        for (uint32_t i = 0; i < filelist.entries_count; i++) {
                if ((entries[i].type == CDFS_ENTRY_TYPE_FILE) &&
                    (strcmp(entries[i].name, FILE_NAME) == 0)) {
                        file_fad = entries[i].starting_fad;
                        file_sectors = sectors_of(entries[i].size);
                }
        }

        memcpy(&chunk_count, asset_bench_table, sizeof(chunk_count));

        if ((file_fad == 0) || (chunk_count == 0) || (chunk_count > CHUNKS_MOST)) {
                return false;
        }

        memcpy(chunks, asset_bench_table + 4, chunk_count * sizeof(chunk_t));

        /* The chunks follow each other through the file, each within the buffers here */
        uint32_t next_sector = 0;

        for (uint32_t c = 0; c < chunk_count; c++) {
                if ((chunks[c].first_sector != next_sector) ||
                    (chunks[c].stream_bytes > CHUNK_INPUT_MOST) ||
                    (chunks[c].output_bytes > CHUNK_OUTPUT_MOST)) {
                        return false;
                }

                next_sector += sectors_of(chunks[c].stream_bytes);
        }

        if (next_sector > file_sectors) {
                return false;
        }

        /* A mesh of points on a spiral, in 16.16 fixed point */
        for (uint32_t v = 0; v < BATCH_VERTICES; v++) {
                const angle_t a = (angle_t)(v * 97);

                mesh[v].x = fix16_cos(a) * 4;
                mesh[v].y = (fix16_t)(v << 8) - FIX16(4.0);
                mesh[v].z = fix16_sin(a) * 4;
        }

        if (vdp2_tvmd_tv_standard_get() == VDP2_TVMD_TV_STANDARD_PAL) {
                frame_rate = 50;
        }

        vdp_sync_vblank_in_set(vblank_in, NULL);

        return true;
}

/* The run's progress, shared by its steps. */
static struct {
        uint32_t decoders;
        uint32_t next_chunk;
        uint32_t chunks_done;
        uint32_t sectors_done;
        uint32_t chunks_bad;
        /* Sectors of the file asked for by plays, and taken by jobs from the partition's front */
        uint32_t requested;
        uint32_t taken;
        uint32_t arrived;
        /* Batches handed out, to the master or the slave, and batches finished */
        uint32_t next_batch;
        uint32_t batches_done;
        /* The master's own batch, and its next vertex in it */
        uint32_t batch;
        uint32_t vertex;
        /* Spare work: particle steps done by either SH-2, and the master's next particle */
        uint32_t spare_steps;
        uint32_t particle;
        bool finished;
        uint32_t start;
        uint32_t decode_end;
        uint32_t transform_end;
} run;

/*
 * The read-ahead's step: 51 counts the partition's sectors, and once the last piece has arrived
 * and the partition runs low, 10 plays the next piece. The drive files it on its own while the
 * SH-2s work. The data port is not involved, so this waits for no transfer, but no command may go
 * to the CD block while one is open.
 */
static void
read_ahead(void)
{
        uint16_t held;

        if (dispatch_port_busy() || !sh1_sectors(READ_PARTITION, &held)) {
                return;
        }

        run.arrived = run.taken + held;

        if ((run.arrived >= run.requested) && (run.requested < file_sectors) && (held < READ_LOW)) {
                uint32_t piece = file_sectors - run.requested;

                if (piece > READ_PIECE) {
                        piece = READ_PIECE;
                }

                if (sh1_play(file_fad + run.requested, (uint16_t)piece)) {
                        run.requested += piece;
                }
        }
}

/* The chunks whose sectors have all arrived and that no job has taken yet. */
static uint32_t
chunks_waiting(void)
{
        uint32_t waiting = 0;

        for (uint32_t c = run.next_chunk; c < chunk_count; c++) {
                if (run.arrived < chunks[c].first_sector + sectors_of(chunks[c].stream_bytes)) {
                        break;
                }
                waiting++;
        }

        return waiting;
}

/* A batch's matrix: rotations by angles of its own, so every batch is new work. */
static void
batch_matrix(uint32_t batch, fix16_mat43_t *matrix)
{
        fix16_mat43_rotation_set((angle_t)(batch * 37), (angle_t)(batch * 53),
            (angle_t)(batch * 11), matrix);
}

/* The slave's transform job, run on the slave. */
static void
transform_batches(void *work __unused)
{
        fix16_mat43_t matrix;

        for (uint32_t b = slave_batches.first; b < slave_batches.first + slave_batches.count; b++) {
                batch_matrix(b, &matrix);

                for (uint32_t v = 0; v < BATCH_VERTICES; v++) {
                        fix16_mat43_pos3_mul(&matrix, &mesh[v], &slave_transformed[v]);
                }
        }
}

static void
batches_finished(uint32_t count)
{
        run.batches_done += count;

        if (run.batches_done == BATCHES) {
                run.transform_end = frames;
        }
}

/* Gives the slave the next GROUP_BATCHES batches of the transform. */
static void
hand_out_batches(worker_t *worker)
{
        if (run.next_batch >= BATCHES) {
                return;
        }

        uint32_t count = BATCHES - run.next_batch;

        if (count > GROUP_BATCHES) {
                count = GROUP_BATCHES;
        }

        /* Written before the notify, and the slave purges its cache before reading it */
        slave_batches.first = run.next_batch;
        slave_batches.count = count;

        dispatch_job_t *const job = &worker->job;

        job->call = transform_batches;
        job->work = NULL;
        job->state = DISPATCH_IDLE;

        if (!dispatch_submit(worker->processor, job)) {
                return;
        }

        run.next_batch += count;
        worker->kind = JOB_TRANSFORM;
        worker->chunk = count;
        worker->busy = true;
}

/* Gives the next chunk to worker once its sectors are at the partition's front. */
static void
hand_out(worker_t *worker)
{
        if (run.next_chunk >= chunk_count) {
                return;
        }

        const chunk_t *const chunk = &chunks[run.next_chunk];
        const uint32_t sectors = sectors_of(chunk->stream_bytes);

        if ((run.arrived < chunk->first_sector + sectors) || dispatch_port_busy()) {
                return;
        }

        dispatch_job_t *const job = &worker->job;

        job->call = NULL;
        job->work = NULL;
        job->input = worker->input;
        job->input_bytes = chunk->stream_bytes;
        job->input_partition = READ_PARTITION;
        job->input_fad = 0;
        job->output = worker->output;
        job->output_bytes = chunk->output_bytes;
        job->state = DISPATCH_IDLE;

        if (!dispatch_submit(worker->processor, job)) {
                return;
        }

        /* The job takes the sectors out of the partition, so the next chunk is at its front */
        run.taken += sectors;
        worker->kind = JOB_DECODE;
        worker->chunk = run.next_chunk++;
        worker->busy = true;
}

/* Moves particles from to to one step: gravity on the velocity, the velocity on the position,
 * and a bounce off the floor at y = 0 that keeps three quarters of the speed. */
static void
particles_move(particle_t *particles, uint32_t from, uint32_t to)
{
        for (uint32_t i = from; i < to; i++) {
                particle_t *const p = &particles[i];

                p->velocity.y -= FIX16(0.01);
                p->position.x += p->velocity.x;
                p->position.y += p->velocity.y;
                p->position.z += p->velocity.z;

                if (p->position.y < 0) {
                        p->position.y = -p->position.y;
                        p->velocity.y = fix16_mul(-p->velocity.y, FIX16(0.75));
                }
        }
}

/* Every particle at its start: a height and a velocity of its own. */
static void
particles_reset(particle_t *particles)
{
        for (uint32_t i = 0; i < PARTICLES; i++) {
                particles[i].position.x = 0;
                particles[i].position.y = (fix16_t)((i & 63) << 12);
                particles[i].position.z = 0;
                particles[i].velocity.x = (fix16_t)(((int32_t)(i & 31) - 16) * 256);
                particles[i].velocity.y = 0;
                particles[i].velocity.z = (fix16_t)(((int32_t)((i >> 5) & 31) - 16) * 256);
        }
}

/* The slave's spare job, run on the slave. */
static void
spare_steps(void *work __unused)
{
        for (uint32_t s = 0; s < FILL_STEPS; s++) {
                particles_move(slave_particles, 0, PARTICLES);
        }
}

static void
hand_out_spare(worker_t *worker)
{
        dispatch_job_t *const job = &worker->job;

        job->call = spare_steps;
        job->work = NULL;
        job->state = DISPATCH_IDLE;

        if (!dispatch_submit(worker->processor, job)) {
                return;
        }

        worker->kind = JOB_SPARE;
        worker->chunk = FILL_STEPS;
        worker->busy = true;
}

/*
 * The slave's next job: a chunk when one waits, the transform otherwise, spare work when neither.
 * With the SH-1 also decoding, the slave leaves the chunks to it until they pile up.
 */
static void
hand_out_slave(worker_t *worker)
{
        const uint32_t backlog = ((run.decoders & (1u << DISPATCH_SH1)) != 0) ? SLAVE_BACKLOG : 1;

        if (((run.decoders & (1u << DISPATCH_SLAVE)) != 0) && (chunks_waiting() >= backlog)) {
                hand_out(worker);
        }

        if (!worker->busy) {
                hand_out_batches(worker);
        }

        if (!worker->busy) {
                hand_out_spare(worker);
        }
}

/* Moves worker's job along, and checks its output once it is done. */
static void
collect(worker_t *worker)
{
        if (!worker->busy) {
                return;
        }

        const dispatch_state_t state = dispatch_poll(worker->processor);

        if (state == DISPATCH_RUNNING) {
                return;
        }

        worker->busy = false;

        /* A call's output is never read, so a failed one shows as work missing, and the run ends
         * once it stops making progress */
        if (worker->kind == JOB_TRANSFORM) {
                if (state == DISPATCH_DONE) {
                        batches_finished(worker->chunk);
                }
                return;
        }

        if (worker->kind == JOB_SPARE) {
                if ((state == DISPATCH_DONE) && !run.finished) {
                        run.spare_steps += worker->chunk;
                }
                return;
        }

        const chunk_t *const chunk = &chunks[worker->chunk];

        run.chunks_done++;
        run.sectors_done += sectors_of(chunk->stream_bytes);

        if ((state != DISPATCH_DONE) ||
            (checksum(worker->output, chunk->output_bytes) != chunk->checksum)) {
                run.chunks_bad++;
        }

        if (run.chunks_done == chunk_count) {
                run.decode_end = frames;
        }
}

/* One slice of the master's spare work, SLICE_PARTICLES particles moved, a step counted each time
 * the slices come round. */
static void
spare_slice(void)
{
        particles_move(master_particles, run.particle, run.particle + SLICE_PARTICLES);

        run.particle += SLICE_PARTICLES;

        if (run.particle == PARTICLES) {
                run.particle = 0;
                run.spare_steps++;
        }
}

/* One slice of the master's transform: SLICE_VERTICES vertices through its batch's matrix. A new
 * batch is taken from the same count the slave's groups come from. With none left the master
 * does a slice of spare work instead. */
static void
transform_slice(void)
{
        static fix16_mat43_t matrix;

        if (run.vertex == 0) {
                if (run.next_batch >= BATCHES) {
                        spare_slice();
                        return;
                }

                run.batch = run.next_batch++;
                batch_matrix(run.batch, &matrix);
        }

        for (uint32_t v = run.vertex; v < run.vertex + SLICE_VERTICES; v++) {
                fix16_mat43_pos3_mul(&matrix, &mesh[v], &transformed[v]);
        }

        run.vertex += SLICE_VERTICES;

        if (run.vertex == BATCH_VERTICES) {
                run.vertex = 0;
                batches_finished(1);
        }
}

/* Tenths of a second in a count of frames. */
static uint32_t
tenths(uint32_t count)
{
        return (count * 10 + frame_rate / 2) / frame_rate;
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

/*
 * The run's lines: its name with the total once it is done, and its change against the first
 * run's total (baseline, 0 for the first run itself), then each part's progress and time.
 */
static void
show(unsigned line, uint32_t index, bool final, uint32_t baseline)
{
        const uint32_t now = frames - run.start;
        const uint32_t decode = (run.chunks_done == chunk_count) ? run.decode_end - run.start : now;
        const uint32_t transform =
            (run.batches_done == BATCHES) ? run.transform_end - run.start : now;

        dbgio_printf("\x1b[%u;2H%s\n", line, runs[index].name);

        if (final) {
                const uint32_t total = (decode > transform) ? decode : transform;

                dbgio_printf("\x1b[%u;27H%3lu.%lu s\n", line, (unsigned long)(tenths(total) / 10),
                    (unsigned long)(tenths(total) % 10));

                if (baseline != 0) {
                        /* The change in time against the first run's, negative when faster */
                        const int32_t change =
                            ((int32_t)total - (int32_t)baseline) * 100 / (int32_t)baseline;

                        /* libyaul's dbgio_printf() takes neither %+ nor %%, so sign and percent
                         * go in as characters */
                        dbgio_printf("\x1b[%u;35H%c%2ld%c\n", line, (change < 0) ? '-' : '+',
                            (long)((change < 0) ? -change : change), '%');
                }
        }
        dbgio_printf("\x1b[%u;4HDecode    %4lu/%4lu sectors %3lu.%lu s\n", line + 1,
            (unsigned long)run.sectors_done, (unsigned long)file_sectors,
            (unsigned long)(tenths(decode) / 10), (unsigned long)(tenths(decode) % 10));
        dbgio_printf("\x1b[%u;4HTransform %4lu/%4lu batches %3lu.%lu s\n", line + 2,
            (unsigned long)run.batches_done, (unsigned long)BATCHES,
            (unsigned long)(tenths(transform) / 10), (unsigned long)(tenths(transform) % 10));

        if (final && (run.chunks_bad != 0)) {
                dbgio_printf("\x1b[%u;4H%lu chunks FAILED their checksum\n", line + 3,
                    (unsigned long)run.chunks_bad);
        } else {
                dbgio_printf("\x1b[%u;4HSpare     %4lu particle steps\n", line + 3,
                    (unsigned long)run.spare_steps);
        }

        text_flush();
        vdp2_sync();
}

/* One run: every chunk decoded and checked, every batch transformed. */
static void
run_one(uint32_t index, unsigned line, uint32_t baseline, benchmark_result_t *result)
{
        memset(&run, 0, sizeof(run));
        run.decoders = runs[index].decoders;

        /* The partition emptied, the drive's output connected to its filter */
        (void)sh1_delete(READ_PARTITION, 0, 0xFFFF);
        (void)sh1_connect(READ_PARTITION);

        particles_reset(master_particles);
        particles_reset(slave_particles);

        run.start = frames;

        uint32_t shown = frames;
        uint32_t slices = 0;
        bool working = true;

        /* The frame of the last progress, and the counts then */
        uint32_t progress = frames;
        uint32_t done_before = 0;
        uint32_t batches_before = 0;

        while (working) {
                /* The CD block's side every few slices. The master always has a slice to do, of
                 * the transform or of spare work */
                const bool ask = (slices++ % POLL_SLICES) == 0;

                for (uint32_t p = 0; p < DISPATCH_PROCESSORS; p++) {
                        if ((run.decoders & (1u << p)) == 0) {
                                continue;
                        }

                        if (ask || (p != DISPATCH_SH1)) {
                                collect(&workers[p]);
                        }
                }

                if (ask) {
                        read_ahead();
                }

                for (uint32_t p = 0; p < DISPATCH_PROCESSORS; p++) {
                        if (((run.decoders & (1u << p)) != 0) && !workers[p].busy) {
                                if (p == DISPATCH_SLAVE) {
                                        hand_out_slave(&workers[p]);
                                } else {
                                        hand_out(&workers[p]);
                                }
                        }
                }

                transform_slice();

                if (frames != shown) {
                        shown = frames;
                        show(line, index, false, baseline);
                }

                working = (run.chunks_done < chunk_count) || (run.batches_done < BATCHES);

                /* A processor lost or a read that never lands: no progress for STALL_FRAMES ends
                 * the run, its missing chunks counted as failed */
                if ((run.chunks_done != done_before) || (run.batches_done != batches_before)) {
                        done_before = run.chunks_done;
                        batches_before = run.batches_done;
                        progress = frames;
                } else if (working && ((frames - progress) > STALL_FRAMES)) {
                        run.chunks_bad += chunk_count - run.chunks_done;
                        if (run.chunks_done < chunk_count) {
                                run.decode_end = frames;
                        }
                        if (run.batches_done < BATCHES) {
                                run.transform_end = frames;
                        }
                        working = false;
                }
        }

        /* The slave may still be on spare work, which ends after the run and is not counted */
        run.finished = true;

        const uint32_t drain = frames;

        while (workers[DISPATCH_SLAVE].busy && ((frames - drain) <= STALL_FRAMES)) {
                collect(&workers[DISPATCH_SLAVE]);
        }

        sh1_disconnect();
        (void)sh1_delete(READ_PARTITION, 0, 0xFFFF);

        result->decode_frames = run.decode_end - run.start;
        result->transform_frames = run.transform_end - run.start;
        result->chunks_bad = run.chunks_bad;

        show(line, index, true, baseline);
}

void
benchmark_run(unsigned first_line, benchmark_result_t results[BENCHMARK_RUNS])
{
        uint32_t baseline = 0;

        for (uint32_t index = 0; index < BENCHMARK_RUNS; index++) {
                const unsigned line = first_line + 5 * index;
                bool ready = true;

                for (uint32_t p = 0; p < DISPATCH_PROCESSORS; p++) {
                        if (((runs[index].decoders & (1u << p)) != 0) && !dispatch_available(p)) {
                                ready = false;
                        }
                }

                if (!ready) {
                        dbgio_printf("\x1b[%u;2H%s: not available\n", line, runs[index].name);
                        text_flush();
                        vdp2_sync();
                        continue;
                }

                run_one(index, line, baseline, &results[index]);

                if (index == 0) {
                        baseline = (results[0].decode_frames > results[0].transform_frames) ?
                            results[0].decode_frames : results[0].transform_frames;
                }
        }
}
