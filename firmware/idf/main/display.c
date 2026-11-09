#include "display.h"
#include "glyphs.h"

#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

enum {
    WIDTH = 128,
    HEIGHT = 160,
    PIN_RESET = 3,
    PIN_CLOCK = 4,
    PIN_MOSI = 9,
    PIN_CS = 10,
    PIN_DC = 8,
    INK = 0x10e7,
    WHITE = 0xffff,
    LINE_PITCH = 16,
};

static const char *TAG = "display";
static spi_device_handle_t spi;
static uint16_t fb[WIDTH * HEIGHT];

extern const uint8_t idle_rgb565_start[] asm("_binary_idle_rgb565_start");
extern const uint8_t idle_rgb565_end[] asm("_binary_idle_rgb565_end");

static void spi_bytes(const uint8_t *data, int len)
{
    while (len > 0) {
        int chunk = len > 1024 ? 1024 : len;
        spi_transaction_t tx = {
            .length = (size_t)chunk * 8,
            .tx_buffer = data,
        };
        ESP_ERROR_CHECK(spi_device_polling_transmit(spi, &tx));
        data += chunk;
        len -= chunk;
    }
}

static void command(uint8_t opcode, const uint8_t *params, size_t count)
{
    gpio_set_level(PIN_DC, 0);
    gpio_set_level(PIN_CS, 0);
    spi_bytes(&opcode, 1);
    if (count) {
        gpio_set_level(PIN_DC, 1);
        spi_bytes(params, count);
    }
    gpio_set_level(PIN_CS, 1);
}

#define CONFIG_CMD(cmd, ...)                                      \
    do {                                                          \
        const uint8_t params[] = {__VA_ARGS__};                   \
        command((cmd), params, sizeof(params));                  \
    } while (0)

static void panel_init(void)
{
    gpio_config_t out = {
        .pin_bit_mask = (1ull << PIN_RESET) | (1ull << PIN_CS) | (1ull << PIN_DC),
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&out));
    gpio_set_level(PIN_RESET, 1);
    gpio_set_level(PIN_CS, 1);
    gpio_set_level(PIN_DC, 1);

    spi_bus_config_t bus = {
        .mosi_io_num = PIN_MOSI,
        .miso_io_num = -1,
        .sclk_io_num = PIN_CLOCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 1024,
    };
    // The panel sends 256-byte rows. With DMA disabled the ESP32-C3 SPI
    // driver's host limit is only 64 bytes, so enable DMA for these transfers.
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO));
    spi_device_interface_config_t dev = {
        // v0.1 drove this panel at APB/3 (26.7 MHz); 20 MHz keeps margin
        // and lets the bounce run near v0.1 speed.
        .clock_speed_hz = 20000000,
        .mode = 0,
        .spics_io_num = -1,
        .queue_size = 1,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &dev, &spi));

    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(PIN_RESET, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(PIN_RESET, 1);
    vTaskDelay(pdMS_TO_TICKS(150));

    command(0x01, 0, 0);
    vTaskDelay(pdMS_TO_TICKS(150));
    command(0x11, 0, 0);
    vTaskDelay(pdMS_TO_TICKS(150));

    // Same ST7735R setup as the v0.1 bare-metal initializer.
    CONFIG_CMD(0xb1, 0x01, 0x2c, 0x2d);
    CONFIG_CMD(0xb2, 0x01, 0x2c, 0x2d);
    CONFIG_CMD(0xb3, 0x01, 0x2c, 0x2d, 0x01, 0x2c, 0x2d);
    CONFIG_CMD(0xb4, 0x07);
    CONFIG_CMD(0xc0, 0xa2, 0x02, 0x84);
    CONFIG_CMD(0xc1, 0xc5);
    CONFIG_CMD(0xc2, 0x0a, 0x00);
    CONFIG_CMD(0xc3, 0x8a, 0x2a);
    CONFIG_CMD(0xc4, 0x8a, 0xee);
    CONFIG_CMD(0xc5, 0x0e);
    command(0x20, 0, 0);
    CONFIG_CMD(0x36, 0xc0);
    CONFIG_CMD(0x3a, 0x05);
    command(0x13, 0, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    command(0x29, 0, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
}

static void put(int x, int y, uint16_t color)
{
    if ((unsigned)x >= WIDTH || (unsigned)y >= HEIGHT)
        return;
    fb[y * WIDTH + x] = color;
}

static const Glyph *find_glyph(uint16_t codepoint)
{
    unsigned start = 0;
    unsigned end = GLYPH_COUNT;
    while (start < end) {
        unsigned middle = start + (end - start) / 2;
        if (glyphs[middle].codepoint == codepoint)
            return &glyphs[middle];
        if (glyphs[middle].codepoint < codepoint)
            start = middle + 1;
        else
            end = middle;
    }
    return 0;
}

bool display_has_glyph(uint32_t codepoint)
{
    if (codepoint > 0xffff)
        return false;
    return find_glyph((uint16_t)codepoint) != 0;
}

static int draw_glyph(int x, int y, const Glyph *glyph, uint16_t color)
{
    for (int row = 0; row < GLYPH_HEIGHT; ++row) {
        unsigned bits = ((unsigned)glyph->bits[row * 2] << 8) | glyph->bits[row * 2 + 1];
        for (int col = 0; col < glyph->width && col < 16; ++col) {
            if (bits & (1u << (15 - col)))
                put(x + col, y + row, color);
        }
    }
    return glyph->advance;
}

static uint32_t utf8_next(const char **text)
{
    const unsigned char *bytes = (const unsigned char *)*text;
    unsigned first = *bytes;
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
    return codepoint;
}

// ---- v0.1 oval bubble, 12px glyph set, at most two lines -----------------

enum {
    CAKE = 0x1f382,
    CAKE_ADVANCE = 14,
    BUBBLE_AREA = 58,
    BUBBLE_LINE_WIDTH = 104, // keep in sync with tools/gen_idf_assets.py
    BUBBLE_MAX_LINES = 2,
    BUBBLE_MAX_GLYPHS = 32,
    BUBBLE_PITCH = SMALL_HEIGHT + 2,
    BUBBLE_INK = 0x18ae, // dark navy outline and text
};

typedef struct {
    uint32_t symbols[BUBBLE_MAX_GLYPHS];
    int count;
    int width;
} BubbleLine;

typedef struct {
    BubbleLine lines[BUBBLE_MAX_LINES];
    int count;
    int width;
} BubblePage;

static const SmallGlyph *find_small(uint32_t c)
{
    if (c > 0xffff)
        return 0;
    int lo = 0, hi = SMALL_COUNT - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (small_glyphs[mid].codepoint == c)
            return &small_glyphs[mid];
        if (small_glyphs[mid].codepoint < c)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return 0;
}

static bool break_after(uint32_t c)
{
    return c == 0xff0c || c == 0x3002 || c == 0xff01 || c == 0xff1f || c == 0x3001 || c == 0xff1b ||
           c == 0xff1a || c == 0x2026 || c == 0xff09 || c == ')' || c == '~';
}

static int bubble_advance(uint32_t c)
{
    if (c == CAKE)
        return CAKE_ADVANCE;
    const SmallGlyph *g = find_small(c);
    if (!g)
        g = find_small('?');
    return g ? g->advance : 8;
}

// Mirror of layout_bubble() in tools/gen_idf_assets.py: a segment that does
// not fit one line is split once, preferably right after punctuation, and
// both halves must fit. Returns false when the text needs more than 2 lines.
static bool layout_bubble(BubblePage *page, const char *text)
{
    memset(page, 0, sizeof(*page));
    uint32_t seg[BUBBLE_MAX_GLYPHS * 2];
    while (*text) {
        int n = 0;
        while (*text && *text != '\n') {
            uint32_t c = utf8_next(&text);
            if (!c)
                break;
            if (c == '\r')
                continue;
            if (n >= (int)(sizeof(seg) / sizeof(seg[0])))
                return false;
            seg[n++] = c;
        }
        if (*text == '\n')
            ++text;
        int start = 0;
        while (start < n && seg[start] == ' ')
            ++start;
        while (n > start && seg[n - 1] == ' ')
            --n;
        if (start >= n)
            continue;
        int total = 0;
        for (int i = start; i < n; ++i)
            total += bubble_advance(seg[i]);
        int cut = n;
        if (total > BUBBLE_LINE_WIDTH) {
            // Cut priority: right after punctuation, then at a space, then any
            // position that is not inside a parenthesised kaomoji.
            bool inside[sizeof(seg) / sizeof(seg[0])];
            int depth = 0;
            for (int i = start; i < n; ++i) {
                inside[i] = depth > 0;
                if (seg[i] == '(' || seg[i] == 0xff08)
                    ++depth;
                else if ((seg[i] == ')' || seg[i] == 0xff09) && depth)
                    --depth;
            }
            int best[3] = {-1, -1, -1}, best_worst = 0;
            int left = total;
            for (int i = n - 1; i > start; --i) {
                left -= bubble_advance(seg[i]);
                if (inside[i])
                    continue;
                int r = i, right = 0;
                while (r < n && seg[r] == ' ')
                    ++r;
                for (int k = r; k < n; ++k)
                    right += bubble_advance(seg[k]);
                if (left > BUBBLE_LINE_WIDTH || right > BUBBLE_LINE_WIDTH)
                    continue;
                int rank = break_after(seg[i - 1]) ? 0 : seg[i] == ' ' ? 1 : 2;
                int worst = left > right ? left : right;
                if (rank == 2) {
                    // No natural break: keep the two lines as even as possible.
                    if (best[2] < 0 || worst < best_worst) {
                        best[2] = i;
                        best_worst = worst;
                    }
                } else if (best[rank] < 0) {
                    best[rank] = i;
                }
            }
            cut = best[0] >= 0 ? best[0] : best[1] >= 0 ? best[1] : best[2];
            if (cut < 0)
                return false;
        }
        for (int part = 0; part < (cut < n ? 2 : 1); ++part) {
            int a = part ? cut : start, b = part ? n : cut;
            while (part && a < b && seg[a] == ' ')
                ++a;
            if (page->count >= BUBBLE_MAX_LINES || b - a > BUBBLE_MAX_GLYPHS)
                return false;
            BubbleLine *line = &page->lines[page->count++];
            for (int k = a; k < b; ++k) {
                line->symbols[line->count++] = seg[k];
                line->width += bubble_advance(seg[k]);
            }
            if (line->width > page->width)
                page->width = line->width;
        }
    }
    return page->count > 0;
}

bool display_bubble_fits(const char *text)
{
    BubblePage page;
    if (!layout_bubble(&page, text))
        return false;
    for (int i = 0; i < page.count; ++i)
        for (int k = 0; k < page.lines[i].count; ++k) {
            uint32_t c = page.lines[i].symbols[k];
            if (c != CAKE && !find_small(c))
                return false;
        }
    return true;
}

// Same integer ellipse test as v0.1 (doubled coordinates, 2px outline).
static bool ellipse_contains(int x, int y, int width, int height, bool inset)
{
    // Fits in 32 bits for a 128x58 bubble (255^2 * 57^2 < 2^31), like v0.1.
    int32_t dx = 2 * x - (width - 1);
    int32_t dy = 2 * y - (height - 1);
    int32_t rx = width - 1 - (inset ? 4 : 0);
    int32_t ry = height - 1 - (inset ? 4 : 0);
    return dx * dx * ry * ry + dy * dy * rx * rx <= rx * rx * ry * ry;
}

// v0.1 cake, 12x13 px.
static void draw_cake(int x, int y)
{
    put(x + 5, y + 2, 0xfda0);
    for (int r = 3; r <= 5; ++r)
        put(x + 5, y + r, 0xd14a);
    for (int r = 6; r <= 11; ++r)
        for (int c = 1; c < 11; ++c)
            put(x + c, y + r, r <= 7 ? 0xfb76 : 0xf697);
    for (int c = 0; c < 12; ++c)
        put(x + c, y + 12, BUBBLE_INK);
}

static void draw_small(int x, int y, const SmallGlyph *g)
{
    for (int row = 0; row < SMALL_HEIGHT; ++row) {
        unsigned bits = ((unsigned)g->bits[row * 2] << 8) | g->bits[row * 2 + 1];
        for (int col = 0; col < g->width && col < 16; ++col)
            if (bits & (1u << (15 - col)))
                put(x + col, y + row, BUBBLE_INK);
    }
}

static void draw_bubble(const char *text)
{
    BubblePage page;
    if (!layout_bubble(&page, text)) {
        // Too long for two lines (should not happen: lines are validated).
        layout_bubble(&page, "(´・ω・`)");
    }
    if (!page.count)
        return;
    int w = page.width + (page.count > 1 ? 26 : 30);
    w = w < 64 ? 64 : (w > WIDTH ? WIDTH : w);
    int h = page.count > 1 ? (w > 110 ? 56 : 50) : 36;
    h = h > BUBBLE_AREA ? BUBBLE_AREA : h;
    int left = (WIDTH - w) / 2;
    int top = (BUBBLE_AREA - h) / 2;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            if (!ellipse_contains(x, y, w, h, false))
                continue;
            put(left + x, top + y, ellipse_contains(x, y, w, h, true) ? WHITE : BUBBLE_INK);
        }
    int lines_h = page.count * BUBBLE_PITCH - 2;
    for (int i = 0; i < page.count; ++i) {
        const BubbleLine *line = &page.lines[i];
        int x = (WIDTH - line->width) / 2;
        int y = top + (h - lines_h) / 2 + i * BUBBLE_PITCH;
        for (int k = 0; k < line->count; ++k) {
            uint32_t c = line->symbols[k];
            if (c == CAKE) {
                draw_cake(x, y);
                x += CAKE_ADVANCE;
                continue;
            }
            const SmallGlyph *g = find_small(c);
            if (!g)
                g = find_small('?');
            if (!g) {
                x += 8;
                continue;
            }
            draw_small(x, y, g);
            x += g->advance;
        }
    }
}

// ---- v0.1 squash-and-rebound ---------------------------------------------

typedef struct {
    int width;  // percent, centred
    int height; // percent, feet stay on the bottom edge
} Pose;

// Exactly the v0.1 table (two short presses in about a third of a second).
static const Pose bounce[] = {
    {101, 97}, {103, 93}, {102, 96}, {100, 103}, {100, 100},
    {104, 91}, {104, 90}, {102, 96}, {99, 104}, {100, 100},
};

static void paint_portrait(const Pose *pose)
{
    size_t expect = WIDTH * HEIGHT * 2;
    size_t have = (size_t)(idle_rgb565_end - idle_rgb565_start);
    if (have < expect) {
        for (int i = 0; i < WIDTH * HEIGHT; ++i)
            fb[i] = WHITE;
        return;
    }
    int columns[WIDTH];
    for (int vx = 0; vx < WIDTH; ++vx) {
        int src = pose->width == 100 ? vx : (vx - WIDTH / 2) * 100 / pose->width + WIDTH / 2;
        columns[vx] = (src >= 0 && src < WIDTH) ? src : -1;
    }
    for (int vy = 0; vy < HEIGHT; ++vy) {
        int src_vy = pose->height == 100 ? vy : HEIGHT - 1 - (HEIGHT - 1 - vy) * 100 / pose->height;
        uint16_t *out = &fb[vy * WIDTH];
        if (src_vy < 0 || src_vy >= HEIGHT) {
            for (int vx = 0; vx < WIDTH; ++vx)
                out[vx] = WHITE;
            continue;
        }
        int memory_y = HEIGHT - 1 - src_vy;
        for (int vx = 0; vx < WIDTH; ++vx) {
            int sx = columns[vx];
            if (sx < 0) {
                out[vx] = WHITE;
                continue;
            }
            unsigned at = (unsigned)(memory_y * WIDTH + (WIDTH - 1 - sx)) * 2;
            out[vx] = ((uint16_t)idle_rgb565_start[at] << 8) | idle_rgb565_start[at + 1];
        }
    }
}

// ---- v0.5 home screen ------------------------------------------------------

enum {
    NAVY = 0x18ae,       // #1a1573-ish, matches the character's hair outline
    SOFT_BLUE = 0x8d9d,  // #8fb3e8
    PALE = 0xf7df,       // #f4f8ff card background
    MUTED = 0x5acf,      // date / secondary text
    SUN_GOLD = 0xfe40,
    SUN_ORANGE = 0xfc80,
    CLOUD = 0xd6db,
    RAIN_BLUE = 0x4c3f,
};

static bool in_round_rect(int x, int y, int w, int h, int r)
{
    if (x < 0 || y < 0 || x >= w || y >= h)
        return false;
    int cx = x < r ? r : (x >= w - r ? w - 1 - r : x);
    int cy = y < r ? r : (y >= h - r ? h - 1 - r : y);
    int dx = x - cx, dy = y - cy;
    return dx * dx + dy * dy <= r * r;
}

static void round_rect(int x0, int y0, int w, int h, int r, uint16_t fill, uint16_t border)
{
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            if (!in_round_rect(x, y, w, h, r))
                continue;
            bool edge = !in_round_rect(x - 1, y, w, h, r) || !in_round_rect(x + 1, y, w, h, r) ||
                        !in_round_rect(x, y - 1, w, h, r) || !in_round_rect(x, y + 1, w, h, r);
            put(x0 + x, y0 + y, edge ? border : fill);
        }
}

static void disc(int cx, int cy, int r, uint16_t color)
{
    for (int y = -r; y <= r; ++y)
        for (int x = -r; x <= r; ++x)
            if (x * x + y * y <= r * r)
                put(cx + x, cy + y, color);
}

static int text_width(const char *text)
{
    int w = 0;
    while (*text) {
        uint32_t c = utf8_next(&text);
        if (!c)
            break;
        const Glyph *g = c > 0xffff ? 0 : find_glyph((uint16_t)c);
        if (!g)
            g = find_glyph('?');
        w += g ? g->advance : 8;
    }
    return w;
}

static int draw_text(int x, int y, const char *text, uint16_t color, bool bold)
{
    while (*text) {
        uint32_t c = utf8_next(&text);
        if (!c)
            break;
        const Glyph *g = c > 0xffff ? 0 : find_glyph((uint16_t)c);
        if (!g)
            g = find_glyph('?');
        if (!g) {
            x += 8;
            continue;
        }
        draw_glyph(x, y, g, color);
        if (bold)
            draw_glyph(x + 1, y, g, color);
        x += g->advance + (bold ? 1 : 0);
    }
    return x;
}

static const BigGlyph *big_glyph(char ch)
{
    for (int i = 0; i < BIG_COUNT; ++i)
        if (big_glyphs[i].ch == ch)
            return &big_glyphs[i];
    return 0;
}

static int big_digit_advance(void)
{
    static int advance;
    if (!advance)
        for (int i = 0; i < BIG_COUNT; ++i)
            if (big_glyphs[i].ch != ':' && big_glyphs[i].advance > advance)
                advance = big_glyphs[i].advance;
    return advance;
}

// Tabular digits so the clock does not wobble as it ticks.
static int draw_big(int x, int y, char ch, bool visible)
{
    const BigGlyph *g = big_glyph(ch);
    if (!g)
        return 0;
    int advance = ch == ':' ? g->advance : big_digit_advance();
    int offset = ch == ':' ? 0 : (advance - 2 - g->width) / 2;
    if (visible)
        for (int row = 0; row < BIG_HEIGHT; ++row) {
            uint32_t bits = ((uint32_t)g->bits[row * 3] << 16) | ((uint32_t)g->bits[row * 3 + 1] << 8) |
                            g->bits[row * 3 + 2];
            for (int col = 0; col < g->width; ++col)
                if (bits & (1u << (BIG_SLOTS - 1 - col)))
                    put(x + offset + col, y + row, NAVY);
        }
    return advance;
}

static void cloud_shape(int x, int y, uint16_t fill)
{
    // Outline first, then fill one pixel smaller.
    disc(x + 7, y + 11, 6, NAVY);
    disc(x + 13, y + 8, 7, NAVY);
    disc(x + 19, y + 12, 5, NAVY);
    for (int yy = y + 11; yy <= y + 17; ++yy)
        for (int xx = x + 7; xx <= x + 19; ++xx)
            put(xx, yy, NAVY);
    disc(x + 7, y + 11, 5, fill);
    disc(x + 13, y + 8, 6, fill);
    disc(x + 19, y + 12, 4, fill);
    for (int yy = y + 11; yy <= y + 16; ++yy)
        for (int xx = x + 7; xx <= x + 19; ++xx)
            put(xx, yy, fill);
}

static void sun_shape(int cx, int cy, int r)
{
    static const int8_t rays[8][2] = {{0, -1}, {1, -1}, {1, 0}, {1, 1}, {0, 1}, {-1, 1}, {-1, 0}, {-1, -1}};
    for (int i = 0; i < 8; ++i)
        for (int k = r + 2; k <= r + 4; ++k) {
            int dx = rays[i][0] * k, dy = rays[i][1] * k;
            if (rays[i][0] && rays[i][1]) {
                dx = dx * 7 / 10;
                dy = dy * 7 / 10;
            }
            put(cx + dx, cy + dy, SUN_ORANGE);
        }
    disc(cx, cy, r, SUN_ORANGE);
    disc(cx, cy, r - 1, SUN_GOLD);
}

// 26x24 weather icon with its top-left at (x, y).
static void draw_icon(int x, int y, int icon)
{
    switch (icon) {
    case ICON_SUN:
        sun_shape(x + 13, y + 12, 6);
        break;
    case ICON_PARTLY:
        sun_shape(x + 9, y + 7, 5);
        cloud_shape(x + 2, y + 3, WHITE);
        break;
    case ICON_CLOUD:
        cloud_shape(x, y + 1, CLOUD);
        break;
    case ICON_FOG:
        for (int i = 0; i < 4; ++i)
            for (int xx = x + 3 + (i & 1) * 3; xx < x + 23 - (i & 1) * 2; ++xx) {
                put(xx, y + 6 + i * 4, SOFT_BLUE);
                put(xx, y + 7 + i * 4, SOFT_BLUE);
            }
        break;
    case ICON_RAIN:
    case ICON_SNOW:
    case ICON_THUNDER:
        cloud_shape(x, y, CLOUD);
        if (icon == ICON_RAIN) {
            for (int i = 0; i < 3; ++i)
                for (int k = 0; k < 3; ++k)
                    put(x + 8 + i * 5 - k / 2, y + 19 + k, RAIN_BLUE);
        } else if (icon == ICON_SNOW) {
            for (int i = 0; i < 3; ++i)
                disc(x + 8 + i * 5, y + 20, 1, SOFT_BLUE);
        } else {
            static const int8_t bolt[][2] = {{14, 17}, {13, 18}, {12, 19}, {11, 20}, {13, 20}, {14, 20},
                                             {13, 21}, {12, 22}, {11, 23}};
            for (unsigned i = 0; i < sizeof(bolt) / sizeof(bolt[0]); ++i) {
                put(x + bolt[i][0], y + bolt[i][1], SUN_ORANGE);
                put(x + bolt[i][0] + 1, y + bolt[i][1], SUN_GOLD);
            }
        }
        break;
    default:
        break;
    }
}

static void paint_home(const UiState *ui)
{
    // Card across the top; the character's face stays clear below it.
    round_rect(3, 2, WIDTH - 6, 60, 9, PALE, SOFT_BLUE);
    int x = 9;
    for (const char *p = ui->clock; *p; ++p)
        x += draw_big(x, 9, *p, *p != ':' || ui->colon);
    // Weather column on the right.
    if (ui->icon != ICON_NONE)
        draw_icon(WIDTH - 34, 2, ui->icon);
    if (ui->temp[0]) {
        int tw = text_width(ui->temp) + 1;
        draw_text(WIDTH - 8 - tw, 25, ui->temp, NAVY, true);
    }
    // Hairline divider, then date (left) and condition (right).
    for (int xx = 10; xx < WIDTH - 10; ++xx)
        put(xx, 42, 0xdf1f);
    int cw = ui->cond[0] ? text_width(ui->cond) + 1 : 0;
    char date[sizeof(ui->date)];
    snprintf(date, sizeof(date), "%s", ui->date);
    if (9 + text_width(date) + 6 > WIDTH - 9 - cw) {
        char *space = strchr(date, ' ');
        if (space)
            *space = 0;  // drop the weekday rather than overlap
    }
    draw_text(9, 44, date, MUTED, false);
    if (cw)
        draw_text(WIDTH - 9 - cw, 44, ui->cond, NAVY, true);
    if (ui->note[0]) {
        int nw = text_width(ui->note) + 1;
        if (nw > WIDTH - 24)
            nw = WIDTH - 24;
        int pw = nw + 16;
        int px = (WIDTH - pw) / 2;
        round_rect(px, 65, pw, 20, 9, NAVY, NAVY);
        draw_text(px + 8, 67, ui->note, WHITE, false);
    }
}

static void paint_overlay(const UiState *ui)
{
    if (ui->bubble[0]) {
        draw_bubble(ui->bubble);
        return;
    }
    if (ui->home) {
        paint_home(ui);
        return;
    }
    // Status screens (setup hotspot, connecting...): same card style.
    int count = 0;
    for (int line = 0; line < UI_LINES; ++line)
        if (ui->line[line][0])
            ++count;
    if (count > 4)
        count = 4;
    if (!count)
        return;
    round_rect(3, 2, WIDTH - 6, count * LINE_PITCH + 8, 9, PALE, SOFT_BLUE);
    int y = 6;
    int shown = 0;
    for (int line = 0; line < UI_LINES && shown < count; ++line) {
        if (!ui->line[line][0])
            continue;
        draw_text(9, y, ui->line[line], shown == 0 ? NAVY : MUTED, shown == 0);
        y += LINE_PITCH;
        ++shown;
    }
}

static void flush_frame(void)
{
    // Full GRAM window. MADCTL 0xC0 rotates 180 degrees, so bytes on the wire are
    // the visual frame rotated 180 — the same order v0.1 used for the portrait.
    command(0x2a, (uint8_t[]){0, 0, 0, WIDTH - 1}, 4);
    command(0x2b, (uint8_t[]){0, 0, 0, HEIGHT - 1}, 4);
    gpio_set_level(PIN_DC, 0);
    gpio_set_level(PIN_CS, 0);
    uint8_t opcode = 0x2c;
    spi_bytes(&opcode, 1);
    gpio_set_level(PIN_DC, 1);

    uint8_t row[WIDTH * 2];
    for (int memory_y = 0; memory_y < HEIGHT; ++memory_y) {
        int visual_y = HEIGHT - 1 - memory_y;
        for (int memory_x = 0; memory_x < WIDTH; ++memory_x) {
            uint16_t color = fb[visual_y * WIDTH + (WIDTH - 1 - memory_x)];
            row[2 * memory_x] = color >> 8;
            row[2 * memory_x + 1] = color & 0xff;
        }
        spi_bytes(row, sizeof(row));
    }
    gpio_set_level(PIN_CS, 1);
}

// ---- birthday party: cake, candles, fireworks (all procedural) ------------

enum {
    CAKE_X = 92,  // top-left of the cake body
    CAKE_Y = 138,
    CAKE_W = 30,
    CAKE_H = 15,
    FX_FRAME_MS = 40,
    BLOW_FRAMES = 30,
    FIREWORK_FRAMES = 50,
};

static bool background(int x, int y)
{
    if ((unsigned)x >= WIDTH || (unsigned)y >= HEIGHT)
        return false;
    uint16_t c = fb[y * WIDTH + x];
    // Near-white portrait background only, so bursts stay behind the character.
    return (c >> 11) >= 29 && ((c >> 5) & 63) >= 58 && (c & 31) >= 29;
}

static void put_bg(int x, int y, uint16_t color)
{
    if (background(x, y))
        put(x, y, color);
}

static void flame(int x, int y, int phase, int lean)
{
    // Teardrop flame 3 px wide, 5-6 px tall; phase makes it flicker.
    int tall = 5 + (phase & 1);
    for (int r = 0; r < tall; ++r) {
        int dx = r < 2 ? lean * (phase % 3 == 0 ? 1 : 0) : 0;
        uint16_t outer = r < 2 ? 0xfe40 : 0xfc80;
        if (r == 0) {
            put(x + 1 + dx, y + r, 0xfe40);
        } else {
            put(x + dx, y + r, outer);
            put(x + 1 + dx, y + r, r >= 2 ? 0xffe0 : outer);
            put(x + 2 + dx, y + r, outer);
        }
    }
}

// state: 0 lit (flicker by tick), 1 blowing (lean), 2 out (smoke puffs by age)
static void draw_cake_scene(int state, uint32_t tick, int smoke_age)
{
    // Plate.
    for (int x = CAKE_X - 3; x < CAKE_X + CAKE_W + 3; ++x) {
        put(x, CAKE_Y + CAKE_H, 0xc618);
        put(x, CAKE_Y + CAKE_H + 1, 0x9cd3);
    }
    // Sponge with a jam stripe.
    for (int y = CAKE_Y + 4; y < CAKE_Y + CAKE_H; ++y)
        for (int x = CAKE_X; x < CAKE_X + CAKE_W; ++x)
            put(x, y, y == CAKE_Y + 9 ? 0xd14a : 0xfdd6);
    // Cream top with drips.
    for (int y = CAKE_Y; y < CAKE_Y + 4; ++y)
        for (int x = CAKE_X; x < CAKE_X + CAKE_W; ++x)
            put(x, y, WHITE);
    for (int x = CAKE_X + 2; x < CAKE_X + CAKE_W; x += 5) {
        put(x, CAKE_Y + 4, WHITE);
        put(x, CAKE_Y + 5, WHITE);
        put(x + 1, CAKE_Y + 4, WHITE);
    }
    // Outline.
    for (int x = CAKE_X; x < CAKE_X + CAKE_W; ++x)
        put(x, CAKE_Y - 1, BUBBLE_INK);
    for (int y = CAKE_Y; y < CAKE_Y + CAKE_H; ++y) {
        put(CAKE_X - 1, y, BUBBLE_INK);
        put(CAKE_X + CAKE_W, y, BUBBLE_INK);
    }
    // Strawberries.
    for (int i = 0; i < 3; ++i) {
        int x = CAKE_X + 5 + i * 9;
        put(x, CAKE_Y + 1, 0xf800);
        put(x + 1, CAKE_Y + 1, 0xf800);
        put(x, CAKE_Y + 2, 0xd000);
        put(x + 1, CAKE_Y + 2, 0xd000);
    }
    // Three striped candles.
    for (int i = 0; i < 3; ++i) {
        int x = CAKE_X + 7 + i * 7;
        for (int y = CAKE_Y - 8; y < CAKE_Y - 1; ++y) {
            uint16_t c = ((y + i) & 2) ? 0x5d1f : WHITE;
            put(x, y, c);
            put(x + 1, y, c);
        }
        put(x, CAKE_Y - 9, 0x4208); // wick
        if (state == 0) {
            flame(x - 1, CAKE_Y - 15, (int)((tick + i * 2) % 6), 0);
        } else if (state == 1) {
            flame(x - 1, CAKE_Y - 14, (int)((tick + i) % 6), -1);
        } else if (smoke_age >= 0 && smoke_age < 18) {
            // Little grey puffs drifting up and left, fading out.
            int rise = smoke_age;
            uint16_t grey = smoke_age < 8 ? 0x8410 : 0xbdf7;
            int sx = x - rise / 4 + ((rise + i) % 3 == 0 ? 1 : 0);
            int sy = CAKE_Y - 11 - rise;
            disc(sx, sy, smoke_age < 10 ? 1 : 2, grey);
            if (smoke_age > 4)
                put(sx + 2, sy + 3, 0xc618);
        }
    }
}

static const int8_t DIR_COS[12] = {64, 55, 32, 0, -32, -55, -64, -55, -32, 0, 32, 55};
static const int8_t DIR_SIN[12] = {0, 32, 55, 64, 55, 32, 0, -32, -55, -64, -55, -32};
static const uint16_t SPARK[] = {0xfe40, 0xfb56, 0x5d1f, 0x5f73, 0xfc80, 0xf81f};

typedef struct {
    int8_t x, y; // burst centre
} Burst;

// Bursts beside the head when the wish bubble is up, anywhere otherwise.
static const Burst WITH_BUBBLE[] = {{14, 74}, {114, 68}, {16, 104}, {112, 98}, {60, 64}};
static const Burst OPEN_SKY[] = {{30, 22}, {98, 18}, {64, 36}, {16, 72}, {112, 70}};

static void draw_fireworks(int frame, uint32_t seed, bool bubble)
{
    const Burst *spots = bubble ? WITH_BUBBLE : OPEN_SKY;
    for (int k = 0; k < 5; ++k) {
        int start = k * 6;
        int t = frame - start;
        if (t < 0)
            continue;
        const Burst *b = &spots[(k + seed) % 5];
        uint16_t color = SPARK[(k + seed) % (sizeof(SPARK) / sizeof(SPARK[0]))];
        uint16_t color2 = SPARK[(k + seed + 2) % (sizeof(SPARK) / sizeof(SPARK[0]))];
        if (t < 8) {
            // Rocket rising with a short trail.
            int y = HEIGHT - 4 - (HEIGHT - 4 - b->y) * t / 8;
            for (int j = 0; j < 4; ++j)
                put_bg(b->x, y + j * 2, j ? 0xfdd6 : 0xfe40);
            continue;
        }
        int age = t - 8;
        if (age > 20)
            continue;
        int radius = age < 10 ? 3 + age * 2 : 23 - (age - 10) / 2; // fast then slow
        if (age >= 10)
            radius = 23;
        int drop = age * age / 30;
        for (int d = 0; d < 12; ++d) {
            int px = b->x + DIR_COS[d] * radius / 64 / (bubble ? 2 : 1);
            int py = b->y + DIR_SIN[d] * radius / 64 / (bubble ? 2 : 1) + drop;
            uint16_t c = (d & 1) ? color : color2;
            put_bg(px, py, c);
            if (age < 16) {
                put_bg(px + 1, py, c);
                put_bg(px, py + 1, c);
                put_bg(px + 1, py + 1, c);
                // Inner ring.
                put_bg(b->x + DIR_COS[d] * radius / 128 / (bubble ? 2 : 1),
                       b->y + DIR_SIN[d] * radius / 128 / (bubble ? 2 : 1) + drop, 0xffe0);
            }
        }
    }
}

int display_blow_frames(void)
{
    return BLOW_FRAMES;
}

int display_fireworks_frames(void)
{
    return FIREWORK_FRAMES;
}

int display_fx_frame_ms(void)
{
    return FX_FRAME_MS;
}

void display_party_frame(const UiState *ui, int fx, int frame, uint32_t tick)
{
    static const Pose rest = {100, 100};
    paint_portrait(&rest);
    UiState shown = *ui;
    bool bubble = ui->bubble[0] != 0;
    if (fx == PARTY_BLOW || fx == PARTY_SHOW) {
        shown.bubble[0] = 0; // candles first, wish afterwards / show only
        bubble = false;
    }
    if (fx == PARTY_FIREWORKS || fx == PARTY_SHOW)
        draw_fireworks(frame, ui->fx_seq, bubble);
    paint_overlay(&shown);
    if (fx == PARTY_BLOW) {
        // Flames lean and flicker for 10 frames, then go out with smoke.
        if (frame < 10)
            draw_cake_scene(1, tick, -1);
        else
            draw_cake_scene(2, tick, frame - 10);
    } else {
        draw_cake_scene(ui->candles_out ? 2 : 0, tick, -1);
    }
    flush_frame();
}

void display_init(void)
{
    panel_init();
    size_t expect = WIDTH * HEIGHT * 2;
    size_t have = (size_t)(idle_rgb565_end - idle_rgb565_start);
    if (have != expect)
        ESP_LOGE(TAG, "idle image is %u bytes, expected %u", (unsigned)have, (unsigned)expect);
    UiState blank = {0};
    display_show(&blank);
}

void display_show(const UiState *ui)
{
    if (ui->party) {
        display_party_frame(ui, PARTY_IDLE, 0, (uint32_t)(esp_timer_get_time() / 150000));
        return;
    }
    static const Pose rest = {100, 100};
    paint_portrait(&rest);
    paint_overlay(ui);
    flush_frame();
}

int display_anim_frames(void)
{
    return (int)(sizeof(bounce) / sizeof(bounce[0]));
}

void display_anim_frame(const UiState *ui, int frame)
{
    if (frame < 0 || frame >= display_anim_frames())
        frame = display_anim_frames() - 1;
    paint_portrait(&bounce[frame]);
    paint_overlay(ui);
    flush_frame();
}
