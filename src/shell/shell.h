/*
 * Shell: the screen the scroller runs in, made for any small game of its kind.
 *
 * The top of the screen is the shell's, four text rows: a blank one (old TVs crop the picture's
 * edges), the game's name with its toggle's state on the right, a one-line description, and a
 * status line the game writes. Below them the game draws into a bitmap, a picture in video memory
 * where each 16-bit word is one pixel's color.
 *
 * A game with a toggle flips it with the pad's A button. Until the first press the shell flips it
 * by itself every few seconds (auto mode), so a run nobody watches still shows both states.
 *
 * The game's loop: draw, then shell_frame(), which reads the pad, writes the text rows and waits
 * for the next frame.
 */

#ifndef SHELL_H
#define SHELL_H

#include <stdbool.h>
#include <stdint.h>

/* The game's area: the screen below the text rows, 320 by 192 pixels. */
#define SHELL_WIDTH 320
#define SHELL_HEIGHT 192

/* The bitmap's row length in pixels. Wider than the screen, so it can scroll sideways while the
 * game draws the columns still off screen. The game's area is rows 0 to SHELL_HEIGHT - 1. */
#define SHELL_BITMAP_WIDTH 512

/*
 * What the shell shows for a game. The name and the toggle's state share a row of 38 columns:
 * names up to 17 characters, labels up to 10.
 * - toggle_labels: the toggle's two states, off first. Both NULL for a game without one.
 */
typedef struct {
        const char *name;
        const char *description;
        const char *toggle_labels[2];
} shell_game_t;

/* Sets up the screen, the text rows and the pad, once, before anything else. */
void shell_init(const shell_game_t *game);

/*
 * The bitmap, SHELL_BITMAP_WIDTH pixels a row, each pixel 15-bit color with the top bit set
 * (libyaul's RGB1555(1, r, g, b).raw, 5 bits a component). Writes show as soon as they land, so a
 * game draws columns while they are off screen. While the screen is being drawn, each write waits
 * for one of the video chip's memory slots kept for the CPU.
 */
uint16_t *shell_bitmap(void);

/* Scrolls the game's area to start at bitmap column x (taken modulo the bitmap's width), from
 * the next frame. */
void shell_scroll(uint32_t x);

/* The status line, printf-style, cut to the row. Kept until the next call. */
void shell_status(const char *format, ...) __attribute__((format(printf, 1, 2)));

/* The toggle's state: false off, true on. Always false without one. */
bool shell_toggle(void);

/* Ends the game's frame: reads the pad, flips the toggle on a press or in auto mode, writes the
 * text rows, then waits for the next frame, when the scroll shows. */
void shell_frame(void);

#endif /* SHELL_H */
