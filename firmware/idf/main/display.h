#ifndef COMPANION_DISPLAY_H
#define COMPANION_DISPLAY_H

#include <stdbool.h>
#include <stdint.h>

enum { UI_LINES = 8, UI_COLS = 40 };

typedef struct {
    char line[UI_LINES][UI_COLS];
    // v0.5: oval speech bubble above the character (v0.1 style). When set, the
    // text strips are hidden. "\n" breaks lines; U+1F382 draws a small cake.
    char bubble[96];
    // Bumped once per sweep; the UI plays the v0.1 squash-and-rebound when it changes.
    uint32_t anim_seq;
    // v0.5 home screen (companion mode, online). When home is set the card
    // layout below is drawn instead of the text strips.
    bool home;
    bool colon;      // blinking clock colon
    char clock[6];   // "21:09"
    char date[24];   // "11月9日 周一"
    char temp[12];   // "18°C"
    char cond[16];   // "多云"
    int icon;        // UiIcon
    char note[40];   // optional pill: reminder, message, countdown, IP
    // Birthday party screen: cake with candles, fireworks, blow-out.
    bool party;
    bool candles_out;
    uint32_t fx_seq;   // bump: play fireworks
    uint32_t blow_seq; // bump: blow out candles, then fireworks + wish
} UiState;

enum { PARTY_IDLE, PARTY_BLOW, PARTY_FIREWORKS, PARTY_SHOW };

enum { ICON_NONE, ICON_SUN, ICON_PARTLY, ICON_CLOUD, ICON_FOG, ICON_RAIN, ICON_SNOW, ICON_THUNDER };

void display_init(void);
// Draw over the v0.1 portrait. Empty lines are skipped. Orientation matches the
// bare-metal image: visual top-left is the text area above the character.
void display_show(const UiState *ui);
// True when the small status font can draw this codepoint.
bool display_has_glyph(uint32_t codepoint);
// True when text renders in the oval bubble in at most two lines with known glyphs.
bool display_bubble_fits(const char *text);
// Birthday effects. fx is PARTY_*; frame counts from 0; tick drives flicker.
int display_blow_frames(void);
int display_fireworks_frames(void);
int display_fx_frame_ms(void);
void display_party_frame(const UiState *ui, int fx, int frame, uint32_t tick);
// Number of v0.1 bounce frames, and one frame of it (portrait only; the
// bubble stays still, as in v0.1).
int display_anim_frames(void);
void display_anim_frame(const UiState *ui, int frame);

#endif
