#ifndef COMPANION_ASSETS_H
#define COMPANION_ASSETS_H

#include <stdbool.h>
#include <stdint.h>

// Loads the DFYA pack from the assets partition. Missing or corrupt packs are
// not fatal: every lookup then fails and the screen keeps the built-in portrait.
void assets_init(void);
// Decompressed big-endian RGB565, already rotated 180 like idle.rgb565.
// NULL if this frame is not in the pack (caller draws the built-in portrait).
const uint8_t *assets_frame(int expr, int frame);
int assets_frames(int expr);
bool assets_has(int expr);
int assets_expr_count(void);
// One line for STATUS, and the short note on the settings page.
const char *assets_status_line(void);
void assets_print(void);
void assets_selftest(void);

#endif
