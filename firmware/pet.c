#include "board.h"
#include "bubble.h"
#include "sweep.h"
#include "version.h"

typedef struct {
    uint16_t codepoint;
    uint8_t width;
    uint8_t bitmap[GLYPH_WIDTH * GLYPH_HEIGHT / 4];
} Glyph;

#include "pet_assets.inc"

enum {
    BUBBLE_REST_AREA = 48,
    BUBBLE_AREA = 52,
    SPRITE_TOP = 38,
    SPRITE_BOTTOM = HEIGHT,
    INK = 0x10e7,
    WHITE = 0xffff,
    COMMAND_MAX = 192,
    PAGE_MS = 3200,
    FRAME_TICKS = 16000000 / 30,
};

static const Glyph *find_glyph(uint32_t codepoint)
{
    unsigned start = 0;
    unsigned end = sizeof(glyphs) / sizeof(glyphs[0]);
    while (start < end) {
        const unsigned middle = start + (end - start) / 2;
        if (glyphs[middle].codepoint == codepoint)
            return &glyphs[middle];
        if (glyphs[middle].codepoint < codepoint)
            start = middle + 1;
        else
            end = middle;
    }
    return 0;
}

static const Glyph *glyph_or_fallback(uint32_t codepoint)
{
    const Glyph *glyph = find_glyph(codepoint);
    return glyph ? glyph : find_glyph('?');
}

static unsigned glyph_advance(uint32_t codepoint)
{
    if (codepoint == CAKE || codepoint >= 0x80)
        return GLYPH_WIDTH;
    return glyph_or_fallback(codepoint)->width + 1;
}

static void pixel(uint8_t *row, unsigned x, uint16_t color)
{
    const unsigned offset = 2 * (WIDTH - x - 1);
    row[offset] = color >> 8;
    row[offset + 1] = color & 0xffu;
}

static bool ellipse_contains(unsigned x, unsigned y, unsigned width, unsigned height,
                             bool inset)
{
    const int32_t dx = 2 * (int32_t)x - (int32_t)(width - 1);
    const int32_t dy = 2 * (int32_t)y - (int32_t)(height - 1);
    const int32_t rx = (int32_t)width - 1 - (inset ? 4 : 0);
    const int32_t ry = (int32_t)height - 1 - (inset ? 4 : 0);
    return dx * dx * ry * ry + dy * dy * rx * rx <= rx * rx * ry * ry;
}

static bool inside_bubble(unsigned x, unsigned y, unsigned left,
                          unsigned width, unsigned height)
{
    return x >= left && x < left + width &&
        ellipse_contains(x - left, y, width, height, true);
}

static void draw_cake_row(uint8_t *row, unsigned left, unsigned y,
                          unsigned local_y, unsigned bubble_left,
                          unsigned bubble_width, unsigned bubble_height)
{
    if (y == 2 && inside_bubble(left + 5, local_y, bubble_left, bubble_width, bubble_height))
        pixel(row, left + 5, 0xfda0);
    if (y >= 3 && y <= 5) {
        if (inside_bubble(left + 5, local_y, bubble_left, bubble_width, bubble_height))
            pixel(row, left + 5, 0xd14a);
    }
    if (y >= 6 && y <= 11) {
        const uint16_t color = y <= 7 ? 0xfb76 : 0xf697;
        for (unsigned x = 1; x < 11; ++x)
            if (inside_bubble(left + x, local_y, bubble_left, bubble_width, bubble_height))
                pixel(row, left + x, color);
    }
    if (y == 12) {
        for (unsigned x = 0; x < 12; ++x)
            if (inside_bubble(left + x, local_y, bubble_left, bubble_width, bubble_height))
                pixel(row, left + x, INK);
    }
}

static void draw_text_row(uint8_t *row, const BubbleLine *line, unsigned glyph_row,
                          unsigned local_y, unsigned bubble_left,
                          unsigned bubble_width, unsigned bubble_height)
{
    static const uint16_t shades[] = {WHITE, 0xb5b7, 0x63ae, INK};
    unsigned left = (WIDTH - line->width) / 2;
    for (unsigned i = 0; i < line->count; ++i) {
        const uint32_t codepoint = line->symbols[i];
        if (codepoint == CAKE) {
            draw_cake_row(row, left, glyph_row, local_y, bubble_left,
                          bubble_width, bubble_height);
            left += GLYPH_WIDTH + 1;
            continue;
        }
        const Glyph *glyph = glyph_or_fallback(codepoint);
        for (unsigned x = 0; x < glyph->width; ++x) {
            const unsigned index = glyph_row * GLYPH_WIDTH + x;
            const unsigned shade = glyph->bitmap[index / 4] >> (2 * (index % 4)) & 3u;
            if (shade && inside_bubble(left + x, local_y, bubble_left,
                                       bubble_width, bubble_height))
                pixel(row, left + x, shades[shade]);
        }
        left += glyph->width + 1;
    }
}

static char *append_uint(char *at, unsigned value)
{
    char reversed[10];
    unsigned size = 0;
    do {
        reversed[size++] = '0' + value % 10;
        value /= 10;
    } while (value);
    while (size)
        *at++ = reversed[--size];
    return at;
}

static void report_bubble(unsigned width, unsigned height, unsigned lines)
{
    char line[48] = "BUBBLE ";
    char *at = append_uint(line + 7, width);
    *at++ = ' ';
    at = append_uint(at, height);
    *at++ = ' ';
    at = append_uint(at, lines);
    *at++ = '\r';
    *at++ = '\n';
    *at = 0;
    log_usb(line);
}

static unsigned bubble_width(const BubblePage *page)
{
    const unsigned proposed = page->width + 32;
    return proposed < 64 ? 64 : (proposed > WIDTH ? WIDTH : proposed);
}

static unsigned bubble_height(const BubblePage *page, unsigned width)
{
    const unsigned oval_height = width * 2 / 5;
    const unsigned text_height = page->count > 1 ? 48 : 36;
    const unsigned height = oval_height > text_height ? oval_height : text_height;
    return height > BUBBLE_REST_AREA ? BUBBLE_REST_AREA : height;
}

typedef struct {
    unsigned width;
    unsigned height;
    int bubble_y;
} Pose;

// The reference settles from two short presses in about a third of a second.
static const Pose bounce[] = {
    {101, 97, 2}, {103, 93, 6}, {102, 96, 3}, {100, 103, -3}, {100, 100, 0},
    {104, 91, 8}, {104, 90, 8}, {102, 96, 3}, {99, 104, -3}, {100, 100, 0},
};

static void paint_bubble_row(uint8_t *row, const BubblePage *page, unsigned local_y,
                             unsigned left, unsigned width, unsigned height)
{
    for (unsigned x = 0; x < width; ++x) {
        if (!ellipse_contains(x, local_y, width, height, false))
            continue;
        const bool inside = ellipse_contains(x, local_y, width, height, true);
        pixel(row, left + x, inside ? WHITE : INK);
    }
    for (unsigned line = 0; line < page->count; ++line) {
        const unsigned text_height = page->count * GLYPH_HEIGHT + (page->count - 1) * 2;
        const unsigned start = (height - text_height) / 2 + line * (GLYPH_HEIGHT + 2);
        if (local_y >= start && local_y < start + GLYPH_HEIGHT)
            draw_text_row(row, &page->lines[line], local_y - start, local_y,
                          left, width, height);
    }
}

static void draw_strip(const BubblePage *page, bool paint_bubble, const Pose *pose, unsigned first_row,
                       unsigned last_row, unsigned first_col, unsigned columns)
{
    const unsigned w = bubble_width(page);
    const unsigned h = page->count ? bubble_height(page, w) : 0;
    const unsigned left = (WIDTH - w) / 2;
    const int proposed_top = (BUBBLE_REST_AREA - (int)h) / 2 + pose->bubble_y;
    const unsigned top = proposed_top < 0 ? 0 : (unsigned)proposed_top;
    _Alignas(4) uint8_t row[WIDTH * 2];
    uint16_t source_offsets[WIDTH];
    if (pose->width != 100) {
        for (int i = 0; i < WIDTH; ++i) {
            const int x = WIDTH - i - 1;
            const int column = (x - WIDTH / 2) * 100 / (int)pose->width + WIDTH / 2;
            source_offsets[i] = column >= 0 && column < WIDTH ?
                2 * (WIDTH - column - 1) : UINT16_MAX;
        }
    }

    begin_rectangle(first_col, first_row, columns, last_row - first_row + 1);
    for (int y = (int)last_row; y >= (int)first_row; --y) {
        const unsigned source_row = (HEIGHT - y - 1) * 100 / pose->height;
        if (source_row >= HEIGHT) {
            for (unsigned i = 0; i < sizeof(row); ++i)
                row[i] = 0xff;
        } else {
            const uint8_t *source = idle_art + source_row * sizeof(row);
            if (pose->width == 100) {
                for (unsigned i = 0; i < sizeof(row); i += 4) {
                    uint32_t word;
                    __builtin_memcpy(&word, source + i, sizeof(word));
                    __builtin_memcpy(row + i, &word, sizeof(word));
                }
            } else if (pose->width >= 100) {
                for (unsigned i = 0; i < WIDTH; ++i) {
                    const unsigned offset = source_offsets[i];
                    row[2 * i] = source[offset];
                    row[2 * i + 1] = source[offset + 1];
                }
            } else {
                for (unsigned i = 0; i < WIDTH; ++i) {
                    const unsigned offset = source_offsets[i];
                    row[2 * i] = offset == UINT16_MAX ? 0xff : source[offset];
                    row[2 * i + 1] = offset == UINT16_MAX ? 0xff : source[offset + 1];
                }
            }
        }
        if (paint_bubble && page->count && (unsigned)y >= top && (unsigned)y < top + h)
            paint_bubble_row(row, page, y - top, left, w, h);
        write_pixels(row + 2 * (WIDTH - first_col - columns), 2 * columns);
    }
    end_rectangle();
}

static void draw_scene(const BubblePage *page, int motion_frame)
{
    const unsigned w = bubble_width(page);
    const unsigned h = page->count ? bubble_height(page, w) : 0;
    if (motion_frame < 0) {
        const Pose rest = {100, 100, 0};
        draw_strip(page, true, &rest, 0, BUBBLE_AREA - 1, 0, WIDTH);
        report_bubble(page->count ? w : 0, h, page->count);
        return;
    }
    const Pose *pose = &bounce[motion_frame];
    const unsigned bubble_top = h && h < BUBBLE_REST_AREA ?
        (BUBBLE_REST_AREA - h) / 2 : 0;
    const unsigned portrait_top = bubble_top + h > SPRITE_TOP ? bubble_top + h : SPRITE_TOP;
    // Keep the oval and its lettering still; animate only the portrait beneath it.
    if (portrait_top < SPRITE_BOTTOM)
        draw_strip(page, false, pose, portrait_top, SPRITE_BOTTOM - 1, 0, WIDTH);
}

typedef struct {
    BubblePage page;
    const char *remaining;
    uint32_t deadline;
    bool visible;
} Bubble;

static void show_next_page(Bubble *bubble)
{
    layout_page(&bubble->page, &bubble->remaining, glyph_advance);
    draw_scene(&bubble->page, -1);
    bubble->visible = bubble->page.count != 0;
    bubble->deadline = timer_ticks() + PAGE_MS * SWEEP_TICKS_PER_MS;
}

static void say(Bubble *bubble, const char *message)
{
    bubble->remaining = message;
    show_next_page(bubble);
}

typedef struct {
    char line[COMMAND_MAX];
    char text[COMMAND_MAX];
    unsigned length;
    bool overflow;
} SerialCommands;

typedef struct {
    Bubble bubble;
    unsigned reaction;
    int motion;
    uint32_t motion_start;
    uint32_t next_frame;
    bool testing;
} Pet;

static void react(Pet *pet)
{
    static const char *const reactions[] = {
        "摸到我啦！",
        "今天也开心",
        "累了就歇会儿\n我在这儿陪你",
        "送你个拥抱！",
    };
    log_usb("MOTION START\r\n");
    say(&pet->bubble, reactions[pet->reaction]);
    pet->reaction = (pet->reaction + 1) % (sizeof(reactions) / sizeof(reactions[0]));
    pet->motion = 0;
    pet->motion_start = timer_ticks();
    pet->next_frame = pet->motion_start;
}

static void animate(Pet *pet)
{
    if ((int32_t)(timer_ticks() - pet->next_frame) < 0) {
        delay_ms(1);
        return;
    }
    draw_scene(&pet->bubble.page, pet->motion++);
    pet->next_frame += FRAME_TICKS;
    if ((unsigned)pet->motion == sizeof(bounce) / sizeof(bounce[0])) {
        const unsigned elapsed = (timer_ticks() - pet->motion_start) / SWEEP_TICKS_PER_MS;
        char line[40] = "MOTION DONE ";
        char *at = append_uint(line + 12, sizeof(bounce) / sizeof(bounce[0]));
        *at++ = ' ';
        at = append_uint(at, elapsed);
        *at++ = '\r';
        *at++ = '\n';
        *at = 0;
        log_usb(line);
        pet->motion = -1;
    }
}

static void handle_command(SerialCommands *serial, Pet *pet)
{
    if (serial->overflow)
        return;
    const char *line = serial->line;
    if (serial->length == 4 && line[0] == 'I' && line[1] == 'D' &&
        line[2] == 'L' && line[3] == 'E') {
        say(&pet->bubble, "");
    } else if (serial->length == 3 && line[0] == 'P' && line[1] == 'E' && line[2] == 'T') {
        react(pet);
    } else if (serial->length == 4 && line[0] == 'T' && line[1] == 'E' &&
               line[2] == 'S' && line[3] == 'T') {
        pet->testing = true;
    } else if (serial->length == 4 && line[0] == 'L' && line[1] == 'I' &&
               line[2] == 'V' && line[3] == 'E') {
        pet->testing = false;
    } else if (serial->length > 4 && line[0] == 'S' && line[1] == 'A' &&
               line[2] == 'Y' && line[3] == ' ') {
        for (unsigned i = 4; i < serial->length; ++i)
            serial->text[i - 4] = line[i];
        serial->text[serial->length - 4] = 0;
        say(&pet->bubble, serial->text);
    }
}

static void poll_commands(SerialCommands *serial, Pet *pet)
{
    for (unsigned i = 0; i < 64; ++i) {
        const int byte = usb_read_byte();
        if (byte < 0)
            break;
        if (byte == '\n') {
            handle_command(serial, pet);
            serial->length = 0;
            serial->overflow = false;
        } else if (byte >= 32 && serial->length < sizeof(serial->line)) {
            serial->line[serial->length++] = byte;
        } else if (byte >= 32) {
            serial->overflow = true;
        }
    }
}

void screen_test(void)
{
    delay_ms(100);
    init_display();
    configure_hardware_spi(3);
    init_sonar();

    _Static_assert(sizeof(idle_art) == WIDTH * HEIGHT * 2, "Portrait dimensions");
    begin_rectangle(0, 0, WIDTH, HEIGHT);
    write_pixels(idle_art, sizeof(idle_art));
    end_rectangle();
    log_usb("PET READY " COMPANION_VERSION "\r\n");

    Pet pet;
    pet.reaction = 0;
    pet.motion = -1;
    pet.testing = false;
    SerialCommands serial;
    serial.length = 0;
    serial.overflow = false;
    say(&pet.bubble, "祝你\n生日快乐🎂");

    SweepGate gate;
    gate.first = 0;
    gate.length = 0;
    gate.latched = false;

    for (;;) {
        if (pet.motion >= 0) {
            poll_commands(&serial, &pet);
            animate(&pet);
            continue;
        }
        const uint32_t start = timer_ticks();
        unsigned mm = pet.testing ? 0 : distance_mm();
        if (mm < 20 || mm > 4000)
            mm = 0;
        if (update_sweep(&gate, start, mm)) {
            log_usb("PET SWEEP\r\n");
            react(&pet);
        }
        poll_commands(&serial, &pet);
        if (pet.motion >= 0)
            continue;
        if (pet.bubble.visible && (int32_t)(timer_ticks() - pet.bubble.deadline) >= 0)
            show_next_page(&pet.bubble);

        const uint32_t elapsed = timer_ticks() - start;
        const uint32_t period = 70u * SWEEP_TICKS_PER_MS;
        if (elapsed < period)
            delay_ms((period - elapsed + SWEEP_TICKS_PER_MS - 1) / SWEEP_TICKS_PER_MS);
    }
}
