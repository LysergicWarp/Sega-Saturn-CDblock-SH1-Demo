/*
 * Particle steps on all three CPUs, with the card firmware off the disc, for a console with no MPEG
 * card.
 *
 * Each processor moves particles of its own, PARTICLES each, in its own memory: the master and the
 * slave in work RAM, the SH-1 in its module's BSS in the CD buffer's RAM
 * (src/modules-asm/particle-module.s). They share nothing while they run:
 * - A particle is moved all of a round's STEPS in registers and written back once, so each SH-2
 *   uses the bus about once per STEPS steps, and the two stay out of each other's way.
 * - The SH-1 is on a bus of its own. Nothing crosses the CD block's data port: AB starts each job
 *   of STEPS steps, AC gives its checksum.
 *
 * Four runs of RUN_FRAMES each: the master alone, the slave alone, the SH-1 alone, then all three at
 * once. Each line gives particle steps a second, one particle moved one step being one. The last
 * run gives each processor's rate as a share of its rate alone, and the total as a share of the
 * three rates alone added up.
 *
 * Then the check: each processor resets its particles and moves them CHECK_STEPS steps, and the
 * slave's and the SH-1's checksums must equal the master's. The three run the same steps, the SH-1's
 * in its own code, so equal checksums say it computes what the SH-2s do.
 *
 * The SH-1 comes up as in tests/disc-bench: this disc's header names
 * src/disc-extensions/firmware-boot-asm as its disc extension, E0 runs it, and line 3 shows E1's
 * result and 01's CD block revision and hardware flag. Then 93, AD, and the particle module
 * uploaded.
 */

#include <yaul.h>

#include <string.h>

#include <sh1-client.h>

#define PARTICLES 1280

/* The steps of an SH-1 job, and of an SH-2 round over its particles. */
#define STEPS 16

/* The particles the master moves between looks at the SH-1's job. */
#define BLOCK 128

/* The check's steps. */
#define CHECK_STEPS 64

#define GRAVITY 3

/* Each run's length: 5 s at 60 frames a second, 6 s on a 50 Hz (PAL) console. */
#define RUN_FRAMES 300

/* The particle module's argument: bit 31 resets the particles, the low 16 bits are the steps. */
#define SH1_RESET 0x80000000UL

/* The CD block's interrupt flags register, for E0's end, EFLS, and a job's end, MPED, without a
 * command. */
#define CD_HIRQ (*(volatile uint16_t *)0x25890008UL)
#define HIRQ_EFLS 0x0200

/* Frames E0 may take, past the firmware's own 20 s limit on the disc check. */
#define AUTH_FRAMES 1500

/* The CD block revision the disc extension sets, its byte +0x44. */
#define EXTENSION_REVISION 0xB0

/* Polls before the master gives up on the slave. */
#define SLAVE_POLLS 0x1000000UL

/* The partition the module's package is put through. */
#define PACKAGE_PARTITION 1

enum {
        MASTER,
        SLAVE,
        SH1,
        PROCESSORS
};

static const char *const names[PROCESSORS] = { "Master", "Slave", "SH-1" };

/* A particle as the module keeps it: position and velocity, int16 each. */
typedef struct {
        int16_t x;
        int16_t y;
        int16_t z;
        int16_t vx;
        int16_t vy;
        int16_t vz;
} particle_t;

/* The particle module's package. */
extern const uint8_t asset_particle[];
extern const uint8_t asset_particle_end[];

/* The package is put into the CD block in whole sectors, so its copy is padded to one. */
static uint8_t package[SH1_SECTOR_BYTES] __aligned(16);

/* Each SH-2's particles. Neither touches the other's. */
static particle_t master_particles[PARTICLES];
static particle_t slave_particles[PARTICLES];

/* Video frames since the start, counted at each vertical blank's start, and frames a second. */
static volatile uint32_t frames;
static uint32_t frame_rate = 60;

/*
 * The slave's mailbox, at its cache-through address so each side sees the other's writes.
 * - mode: what the next entry does, taken by the slave.
 * - go: the run lasts while it is set.
 * - rounds: the rounds the slave has finished in the run.
 * - sum: the checksum after the check.
 * - done: set when the slave's run or check has ended.
 */
typedef enum {
        SLAVE_IDLE,
        SLAVE_RUN,
        SLAVE_CHECK
} slave_mode_t;

static struct {
        volatile uint32_t ready;
        volatile uint32_t mode;
        volatile uint32_t go;
        volatile uint32_t rounds;
        volatile uint32_t sum;
        volatile uint32_t done;
} slave_box __uncached;

static bool slave_up;
static bool sh1_up;

/* The particle module's handle, and the ID of the SH-1's last job. */
static uint8_t particle_handle;
static uint16_t job_id;

/* The SH-1's part of a run: whether a job is out, the jobs done, and a failure. */
static struct {
        bool out;
        uint32_t jobs;
        bool failed;
} sh1_run;

static void
vblank_in(void *work __unused)
{
        frames++;
}

/* Ends a frame: the text written so far goes to VDP2 at the next vertical blank. Interrupts are
 * masked across the flush: a vertical blank inside it would leave it unmarked, and libyaul drops
 * every flush after that. */
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

/* Waits for the next vertical blank and returns the frame count it starts. */
static uint32_t
frame_edge(void)
{
        const uint32_t now = frames;

        while (frames == now) {
        }

        return frames;
}

/* Particle i at its start, as the module's reset makes it. */
static void
particles_reset(particle_t *particles)
{
        for (int32_t i = 0; i < PARTICLES; i++) {
                particle_t *const p = &particles[i];

                p->x = 0;
                p->y = (int16_t)((i & 63) * 64);
                p->z = 0;
                p->vx = (int16_t)(((i & 31) - 16) * 4);
                p->vy = 0;
                p->vz = (int16_t)((((i >> 5) & 31) - 16) * 4);
        }
}

/*
 * Moves particles from to to steps steps each, as the module does: vy less GRAVITY, each velocity
 * added to its position, and below the floor y and vy negated and vy less a quarter of itself. Each
 * particle is moved in locals and written back once. x and z keep their low 16 bits, the 16-bit
 * wrap. vy is positive where it is shifted, so the shift is the module's.
 */
static void
particles_move(particle_t *particles, uint32_t from, uint32_t to, uint32_t steps)
{
        for (uint32_t i = from; i < to; i++) {
                particle_t *const p = &particles[i];
                int32_t x = p->x;
                int32_t y = p->y;
                int32_t z = p->z;
                const int32_t vx = p->vx;
                int32_t vy = p->vy;
                const int32_t vz = p->vz;

                for (uint32_t s = 0; s < steps; s++) {
                        vy -= GRAVITY;
                        x += vx;
                        y += vy;
                        z += vz;

                        if (y < 0) {
                                y = -y;
                                vy = -vy;
                                vy -= vy >> 2;
                        }
                }

                p->x = (int16_t)x;
                p->y = (int16_t)y;
                p->z = (int16_t)z;
                p->vy = (int16_t)vy;
        }
}

/* The module's checksum: for each particle's longwords x:y, z:vx and vy:vz, the sum rotated left by
 * 1, then the longword added. */
static uint32_t
particles_sum(const particle_t *particles)
{
        uint32_t sum = 0;

        for (uint32_t i = 0; i < PARTICLES; i++) {
                const particle_t *const p = &particles[i];
                const uint32_t words[3] = {
                        ((uint32_t)(uint16_t)p->x << 16) | (uint16_t)p->y,
                        ((uint32_t)(uint16_t)p->z << 16) | (uint16_t)p->vx,
                        ((uint32_t)(uint16_t)p->vy << 16) | (uint16_t)p->vz
                };

                for (uint32_t k = 0; k < 3; k++) {
                        sum = ((sum << 1) | (sum >> 31)) + words[k];
                }
        }

        return sum;
}

/*
 * Runs on the slave each time the master notifies it (libyaul's polling mode: the slave spins on
 * its notify flag, an on-chip register, so it leaves the bus alone while it waits). Only the slave
 * touches its particles, so its cache needs no purge.
 */
static void
slave_entry(void)
{
        slave_box.ready = 1;

        const uint32_t mode = slave_box.mode;

        if (mode == SLAVE_IDLE) {
                return;
        }

        /* Taken: a second notify finds nothing to do */
        slave_box.mode = SLAVE_IDLE;

        particles_reset(slave_particles);

        if (mode == SLAVE_RUN) {
                uint32_t rounds = 0;

                while (slave_box.go != 0) {
                        particles_move(slave_particles, 0, PARTICLES, STEPS);
                        rounds++;
                        slave_box.rounds = rounds;
                }
        } else {
                particles_move(slave_particles, 0, PARTICLES, CHECK_STEPS);
                slave_box.sum = particles_sum(slave_particles);
        }

        slave_box.done = 1;
}

/* Starts the slave and waits for it to answer, as the dispatch does: a notify sent before the slave
 * reaches its loop is lost, so the master notifies until the slave has set ready once. */
static bool
slave_start(void)
{
        slave_box.mode = SLAVE_IDLE;
        slave_box.ready = 0;

        cpu_dual_comm_mode_set(CPU_DUAL_ENTRY_POLLING);
        cpu_dual_slave_set(slave_entry);

        for (uint32_t i = 0; i < SLAVE_POLLS; i++) {
                cpu_dual_slave_notify();

                if (slave_box.ready != 0) {
                        return true;
                }
        }

        return false;
}

static void
slave_begin(slave_mode_t mode)
{
        slave_box.done = 0;
        slave_box.rounds = 0;
        slave_box.go = 1;
        slave_box.mode = mode;
        cpu_dual_slave_notify();
}

static bool
slave_wait(void)
{
        for (uint32_t i = 0; i < SLAVE_POLLS; i++) {
                if (slave_box.done != 0) {
                        return true;
                }
        }

        return false;
}

/* Ends the slave's run: the rounds finished by now, then go cleared. The round under way still
 * ends, uncounted. */
static uint32_t
slave_stop(void)
{
        const uint32_t rounds = slave_box.rounds;

        slave_box.go = 0;

        if (!slave_wait()) {
                slave_up = false;
        }

        return rounds;
}

/* E0 subcode 0 on partition 0, as the BIOS sends it, then its end, EFLS, a frame at a time. */
static bool
authenticate(void)
{
        CD_HIRQ = (uint16_t)~HIRQ_EFLS;

        if (!sh1_command(0xE000, 0x0000, 0x0000, 0x0000, 0) ||
            ((sh1_answer[0] >> 8) == SH1_STATUS_REJECT)) {
                return false;
        }

        for (uint32_t i = 0; i < AUTH_FRAMES; i++) {
                if ((CD_HIRQ & HIRQ_EFLS) != 0) {
                        return true;
                }

                frame_end();
        }

        return false;
}

/*
 * The SH-1 up from the disc: E0, then E1's result and 01's revision and hardware flag on line 3,
 * then 93, AD, the selector reset and the particle module's load. True once the module is
 * installed, its handle in particle_handle.
 */
static bool
sh1_start(void)
{
        uint8_t result = 0xFF;
        uint8_t revision = 0xFF;
        uint8_t flag = 0xFF;
        const char *failed = NULL;

        dbgio_puts("\x1b[3;2HE0, disc check\n");
        frame_end();

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
                const uint32_t bytes = (uint32_t)(asset_particle_end - asset_particle);

                if ((bytes == 0) || (bytes > sizeof(package))) {
                        failed = "package";
                } else {
                        memcpy(package, asset_particle, bytes);

                        if (!sh1_module_load(PACKAGE_PARTITION, package, 1, ++job_id, &job) ||
                            (job.state != SH1_JOB_DONE)) {
                                failed = "load";
                        } else {
                                particle_handle = (uint8_t)job.result;
                        }
                }
        }

        dbgio_printf("\x1b[3;2HE1 %02X rev %02X hw %02X %-12s\n", result, revision, flag,
            (failed == NULL) ? "SH-1 up" : failed);
        frame_end();

        return (failed == NULL);
}

/* Starts an SH-1 job, MPED cleared first so that its end shows in HIRQ alone. */
static void
sh1_job_start(uint32_t argument)
{
        CD_HIRQ = (uint16_t)~SH1_MPED;

        if (!sh1_submit(particle_handle, ++job_id, argument)) {
                sh1_run.failed = true;
                return;
        }

        sh1_run.out = true;
}

/*
 * A look at the SH-1's job, cheap until MPED: one register read. Once the job has ended, AC
 * confirms it, it is counted, and with more the next one starts.
 */
static void
sh1_service(bool more)
{
        if (!sh1_run.out || ((CD_HIRQ & SH1_MPED) == 0)) {
                return;
        }

        sh1_job_t job;

        if (!sh1_status(&job) || (job.id != job_id)) {
                sh1_run.out = false;
                sh1_run.failed = true;
                return;
        }

        if ((job.state == SH1_JOB_SUBMITTED) || (job.state == SH1_JOB_RUNNING)) {
                return;
        }

        sh1_run.out = false;

        if (job.state != SH1_JOB_DONE) {
                sh1_run.failed = true;
                return;
        }

        sh1_run.jobs++;

        if (more) {
                sh1_job_start(STEPS);
        }
}

/* Waits for the job still out at a run's end. It is not counted. */
static void
sh1_finish(void)
{
        if (!sh1_run.out) {
                return;
        }

        sh1_job_t job;

        if (!sh1_wait(job_id, &job)) {
                sh1_up = false;
        }

        sh1_run.out = false;
}

/* Particle steps a second from steps counted over elapsed frames. */
static uint32_t
rate_of(uint64_t steps, uint32_t elapsed)
{
        return (elapsed == 0) ? 0 : (uint32_t)((steps * frame_rate) / elapsed);
}

/*
 * One run of RUN_FRAMES with the processors asked for, each at its own particles from their start.
 * rates gets each one's steps a second, 0 for one that did not run or failed.
 *
 * The master moves its particles a block at a time and looks at the SH-1's job between blocks.
 * Without particles of its own it only waits: on its frame count, which its cache holds, or with the
 * SH-1 running, on HIRQ.
 */
static void
run(bool master, bool slave, bool sh1, uint32_t rates[PROCESSORS])
{
        memset(rates, 0, PROCESSORS * sizeof(rates[0]));
        memset(&sh1_run, 0, sizeof(sh1_run));

        slave = slave && slave_up;
        sh1 = sh1 && sh1_up;

        if (master) {
                particles_reset(master_particles);
        }

        const uint32_t start = frame_edge();

        if (slave) {
                slave_begin(SLAVE_RUN);
        }

        if (sh1) {
                sh1_job_start(SH1_RESET | STEPS);
        }

        uint64_t master_steps = 0;
        uint32_t block = 0;

        while ((frames - start) < RUN_FRAMES) {
                if (master) {
                        particles_move(master_particles, block, block + BLOCK, STEPS);
                        master_steps += BLOCK * STEPS;
                        block = (block + BLOCK) % PARTICLES;
                }

                if (sh1) {
                        sh1_service(true);
                }
        }

        const uint32_t elapsed = frames - start;
        const uint32_t slave_rounds = slave ? slave_stop() : 0;

        if (sh1) {
                sh1_finish();
        }

        if (master) {
                rates[MASTER] = rate_of(master_steps, elapsed);
        }

        if (slave && slave_up) {
                rates[SLAVE] = rate_of((uint64_t)slave_rounds * PARTICLES * STEPS, elapsed);
        }

        if (sh1 && sh1_up && !sh1_run.failed) {
                rates[SH1] = rate_of((uint64_t)sh1_run.jobs * PARTICLES * STEPS, elapsed);
        }
}

static void
rate_print(unsigned line, uint32_t processor, uint32_t rate, uint32_t alone)
{
        if (rate == 0) {
                dbgio_printf("\x1b[%u;4H%-8s not available\n", line, names[processor]);
                return;
        }

        dbgio_printf("\x1b[%u;4H%-8s %9lu\n", line, names[processor], (unsigned long)rate);

        /* dbgio's printf takes no %%, so the sign goes in through %c */
        if (alone != 0) {
                dbgio_printf("\x1b[%u;24H%3lu%c\n", line,
                    (unsigned long)(((uint64_t)rate * 100 + alone / 2) / alone), '%');
        }
}

/* The check: each processor's checksum after CHECK_STEPS steps from the start. */
static const char *
check_of(bool up, bool got, uint32_t sum, uint32_t reference)
{
        if (!up) {
                return "not run";
        }

        if (!got) {
                return "no answer";
        }

        return (sum == reference) ? "ok" : "DIFFERS";
}

int
main(void)
{
        dbgio_init();
        dbgio_dev_default_init(DBGIO_DEV_VDP2_ASYNC);
        dbgio_dev_font_load();

        if (vdp2_tvmd_tv_standard_get() == VDP2_TVMD_TV_STANDARD_PAL) {
                frame_rate = 50;
        }

        vdp_sync_vblank_in_set(vblank_in, NULL);

        dbgio_puts("\x1b[2;2HParticle steps on three CPUs, no card\n");
        frame_end();

        /* libyaul's cd_block_init() restarts the CD block, so it goes before the SH-1 start */
        cd_block_init();

        slave_up = slave_start();
        sh1_up = sh1_start();

        dbgio_printf("\x1b[4;2H%u particles each, steps a second\n", PARTICLES);
        dbgio_puts("\x1b[6;2HAlone\n");
        dbgio_puts("\x1b[11;2HTogether               of alone\n");
        frame_end();

        uint32_t alone[PROCESSORS];
        uint32_t rates[PROCESSORS];

        for (uint32_t p = 0; p < PROCESSORS; p++) {
                dbgio_printf("\x1b[%lu;4H%-8s running\n", (unsigned long)(7 + p), names[p]);
                frame_end();

                run(p == MASTER, p == SLAVE, p == SH1, rates);
                alone[p] = rates[p];

                rate_print(7 + p, p, alone[p], 0);
                frame_end();
        }

        dbgio_puts("\x1b[12;4Hall three running\n");
        frame_end();

        run(true, true, true, rates);

        uint32_t total = 0;
        uint32_t total_alone = 0;

        for (uint32_t p = 0; p < PROCESSORS; p++) {
                rate_print(12 + p, p, rates[p], alone[p]);
                total += rates[p];
                total_alone += alone[p];
        }

        dbgio_printf("\x1b[15;4H%-8s %9lu\n", "Total", (unsigned long)total);

        if (total_alone != 0) {
                dbgio_printf("\x1b[15;24H%3lu%c\n",
                    (unsigned long)(((uint64_t)total * 100 + total_alone / 2) / total_alone),
                    '%');
        }

        frame_end();

        /* The check: the master's checksum the reference, then the slave's and the SH-1's */
        particles_reset(master_particles);
        particles_move(master_particles, 0, PARTICLES, CHECK_STEPS);

        const uint32_t reference = particles_sum(master_particles);

        bool slave_got = false;
        bool sh1_got = false;
        uint32_t sh1_sum = 0;

        if (slave_up) {
                slave_begin(SLAVE_CHECK);
                slave_got = slave_wait();
        }

        if (sh1_up) {
                sh1_job_t job = { 0 };

                CD_HIRQ = (uint16_t)~SH1_MPED;
                sh1_got = sh1_submit(particle_handle, ++job_id, SH1_RESET | CHECK_STEPS) &&
                    sh1_wait(job_id, &job) && (job.state == SH1_JOB_DONE);
                sh1_sum = job.result;
        }

        dbgio_printf("\x1b[17;2HCheck, %u steps: %08lX\n", CHECK_STEPS, (unsigned long)reference);
        dbgio_printf("\x1b[18;4HSlave %s, SH-1 %s\n",
            check_of(slave_up, slave_got, slave_box.sum, reference),
            check_of(sh1_up, sh1_got, sh1_sum, reference));
        dbgio_printf("\x1b[26;2HEach run %u frames, %lu s.\n", RUN_FRAMES,
            (unsigned long)(RUN_FRAMES / frame_rate));
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
