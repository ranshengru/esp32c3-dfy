#ifndef COMPANION_ASSETS_H
#define COMPANION_ASSETS_H

#include <stdbool.h>
#include <stdint.h>

// Loads the DFYA pack from the assets partition and maps it through the
// flash cache. Frames are raw RGB565 in flash, not a RAM copy. A missing or
// corrupt pack is not fatal: lookups fail and the screen keeps the built-in
// portrait.
void assets_init(void);
// Big-endian RGB565 already rotated 180 like idle.rgb565, pointing into the
// mapped assets partition. NULL if this frame is not in the pack.
const uint8_t *assets_frame(int expr, int frame);
int assets_frames(int expr);
bool assets_has(int expr);
int assets_expr_count(void);
// One line for STATUS, and the short note on the settings page.
const char *assets_status_line(void);
void assets_print(void);
void assets_selftest(void);

#endif
