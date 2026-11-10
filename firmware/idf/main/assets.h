#ifndef COMPANION_ASSETS_H
#define COMPANION_ASSETS_H

#include <stdbool.h>
#include <stdint.h>

// Maps the MJPEG expression clips from the assets partition. A missing or
// old pack is not fatal: blit fails and the screen keeps the built-in portrait.
void assets_init(void);
// JPEG bytes of one video frame inside the mapped partition, or NULL.
const uint8_t *assets_frame(int expr, int frame);
// Decode that frame into a 128x160 RGB565 buffer in visual order (the same
// order as the framebuffer). Uses a few KB of workspace, not a second picture.
bool assets_blit(int expr, int frame, uint16_t *dest);
int assets_frames(int expr);
bool assets_has(int expr);
int assets_expr_count(void);
// One line for STATUS, and the short note on the settings page.
const char *assets_status_line(void);
void assets_print(void);
void assets_selftest(void);

#endif
