#ifndef COMPANION_DISPLAY_H
#define COMPANION_DISPLAY_H

#include <stdbool.h>

enum { UI_LINES = 8, UI_COLS = 40 };

typedef struct {
    char line[UI_LINES][UI_COLS];
} UiState;

void display_init(void);
// Draw over the v0.1 portrait. Empty lines are skipped. Orientation matches the
// bare-metal image: visual top-left is the text area above the character.
void display_show(const UiState *ui);

#endif
