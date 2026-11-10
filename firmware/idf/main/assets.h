#ifndef COMPANION_ASSETS_H
#define COMPANION_ASSETS_H

#include <stdbool.h>
#include <stdint.h>

// Classic portrait in the pack (and the fallback) uses this expression id.
enum { EXPR_CLASSIC = 0xFF };

// Maps the assets partition (format 3, LZ4 strips) or, if the pack is
// missing or corrupt, the built-in compressed classic portrait. Neither
// path keeps a full decoded frame or a compressed copy in RAM.
void assets_init(void);
// One stored source row: 128 big-endian RGB565 pixels, already rotated
// 180 like the old idle.rgb565. expr < 0 or EXPR_CLASSIC selects classic.
// false if this row cannot be decoded (caller should paint white).
bool assets_src_row(int expr, int frame, int memory_y, uint8_t *dst);
int assets_frames(int expr);
bool assets_has(int expr);
int assets_expr_count(void);
// One line for STATUS, and the short note on the settings page.
const char *assets_status_line(void);
void assets_print(void);
void assets_selftest(void);

#endif
