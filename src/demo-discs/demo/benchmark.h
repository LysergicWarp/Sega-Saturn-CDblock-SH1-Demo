/*
 * The disc decode benchmark: the same work four times, with more processors each time.
 *
 * The work, in every run:
 * - Decode: a compressed file on the disc, read by the drive into the CD block's buffer and cut in
 *   chunks, each its own PRS stream and its own job. Every chunk's output is checked against a
 *   checksum made when the disc was built.
 * - Transform: a fixed number of batches of 3D vertices, each batch rotated by its own matrix, the
 *   kind of work a game's master CPU does every frame.
 *
 * The runs differ only in who does what:
 * - Master: the master alone copies each chunk in, decodes it, and transforms in between.
 * - Master + slave: the slave decodes each chunk as it arrives and transforms in between, the
 *   master copies the chunks in and transforms.
 * - Master + SH-1: the CD block's SH-1 decodes inside the CD block, the master copies the output
 *   back and transforms.
 * - All three: the SH-1 decodes and the slave transforms with the master, decoding too only when
 *   chunks pile up faster than the SH-1 takes them.
 */

#ifndef BENCHMARK_H
#define BENCHMARK_H

#include <stdbool.h>
#include <stdint.h>

#define BENCHMARK_RUNS 4

typedef struct {
        /* Emulated time is counted in video frames: 60 a second on an NTSC Saturn, 50 on PAL. */
        uint32_t decode_frames;
        uint32_t transform_frames;
        uint32_t chunks_bad;
} benchmark_result_t;

/* Finds the benchmark's file on the disc and checks its chunk table. False without the file, or
 * with chunks out of order or too big for the buffers. libyaul's cd_block_init() goes first, and
 * the dispatch's dispatch_init() after. */
bool benchmark_init(void);

/* Runs all four, showing each on screen from line first_line as it goes. */
void benchmark_run(unsigned first_line, benchmark_result_t results[BENCHMARK_RUNS]);

#endif /* BENCHMARK_H */
