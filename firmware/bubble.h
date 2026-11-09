#ifndef COMPANION_BUBBLE_H
#define COMPANION_BUBBLE_H

#include <stdbool.h>
#include <stdint.h>

enum {
    GLYPH_WIDTH = 12,
    GLYPH_HEIGHT = 14,
    LINE_WIDTH = 96,
    MAX_LINES = 2,
    MAX_LINE_GLYPHS = 40,
    CAKE = 0x1f382,
};

typedef struct {
    uint32_t symbols[MAX_LINE_GLYPHS];
    unsigned count;
    unsigned width;
} BubbleLine;

typedef struct {
    BubbleLine lines[MAX_LINES];
    unsigned count;
    unsigned width;
} BubblePage;

static uint32_t next_character(const char **text)
{
    const unsigned char *bytes = (const unsigned char *)*text;
    const unsigned first = *bytes;
    if (!first)
        return 0;
    ++*text;
    if (first < 0x80)
        return first;

    unsigned length;
    uint32_t codepoint;
    if (first >= 0xc2 && first <= 0xdf) {
        length = 2;
        codepoint = first & 0x1f;
    } else if (first >= 0xe0 && first <= 0xef) {
        length = 3;
        codepoint = first & 0x0f;
    } else if (first >= 0xf0 && first <= 0xf4) {
        length = 4;
        codepoint = first & 0x07;
    } else {
        return '?';
    }
    for (unsigned i = 1; i < length; ++i) {
        if ((bytes[i] & 0xc0) != 0x80)
            return '?';
        codepoint = (codepoint << 6) | (bytes[i] & 0x3f);
    }
    *text += length - 1;
    if ((length == 3 && codepoint < 0x800) ||
        (length == 4 && codepoint < 0x10000) ||
        (codepoint >= 0xd800 && codepoint <= 0xdfff) || codepoint > 0x10ffff)
        return '?';
    return codepoint;
}

static bool punctuation_break(uint32_t codepoint)
{
    return codepoint == ',' || codepoint == '.' || codepoint == '!' ||
        codepoint == '?' || codepoint == ';' || codepoint == ':' ||
        codepoint == 0x3001 || codepoint == 0x3002 || codepoint == 0xff0c ||
        codepoint == 0xff01 || codepoint == 0xff1f || codepoint == 0xff1b ||
        codepoint == 0xff1a;
}

static void layout_page(BubblePage *page, const char **text, unsigned (*advance)(uint32_t))
{
    page->count = 0;
    page->width = 0;
    for (unsigned row = 0; row < MAX_LINES; ++row) {
        while (**text == ' ' || **text == '\n' || **text == '\r')
            ++*text;
        if (!**text)
            break;
        BubbleLine *line = &page->lines[page->count];
        line->count = 0;
        line->width = 0;
        while (line->count < MAX_LINE_GLYPHS) {
            const char *start = *text;
            const uint32_t codepoint = next_character(text);
            if (!codepoint || codepoint == '\n' || codepoint == '\r')
                break;
            const unsigned width = advance(codepoint);
            if (line->width + width > LINE_WIDTH) {
                *text = start;
                break;
            }
            line->symbols[line->count++] = codepoint;
            line->width += width;
            if (punctuation_break(codepoint))
                break;
        }
        if (line->count) {
            ++page->count;
            if (line->width > page->width)
                page->width = line->width;
        }
    }
}

#endif
