#include "display.h"
#include "glyphs.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

enum {
    WIDTH = 128,
    HEIGHT = 160,
    PIN_RESET = 3,
    PIN_CLOCK = 4,
    PIN_MOSI = 20,
    PIN_CS = 10,
    PIN_DC = 21,
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
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_DISABLED));
    spi_device_interface_config_t dev = {
        .clock_speed_hz = 12000000,
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

static void fill_rect(int x, int y, int w, int h, uint16_t color)
{
    for (int row = y; row < y + h; ++row)
        for (int col = x; col < x + w; ++col)
            put(col, row, color);
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

static int draw_codepoint(int x, int y, uint32_t codepoint, uint16_t color)
{
    if (codepoint > 0xffff)
        codepoint = '?';
    const Glyph *glyph = find_glyph((uint16_t)codepoint);
    if (!glyph)
        glyph = find_glyph('?');
    if (!glyph)
        return 8;
    return draw_glyph(x, y, glyph, color);
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

static void draw_line(int y, const char *text)
{
    int x = 4;
    while (*text && x < WIDTH - 4) {
        uint32_t codepoint = utf8_next(&text);
        if (!codepoint)
            break;
        x += draw_codepoint(x, y, codepoint, INK);
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
    size_t expect = WIDTH * HEIGHT * 2;
    size_t have = (size_t)(idle_rgb565_end - idle_rgb565_start);
    if (have >= expect) {
        for (int visual_y = 0; visual_y < HEIGHT; ++visual_y) {
            int memory_y = HEIGHT - 1 - visual_y;
            for (int visual_x = 0; visual_x < WIDTH; ++visual_x) {
                int memory_x = WIDTH - 1 - visual_x;
                unsigned src = (unsigned)(memory_y * WIDTH + memory_x) * 2;
                fb[visual_y * WIDTH + visual_x] =
                    ((uint16_t)idle_rgb565_start[src] << 8) | idle_rgb565_start[src + 1];
            }
        }
    } else {
        for (int i = 0; i < WIDTH * HEIGHT; ++i)
            fb[i] = WHITE;
    }

    int y = 2;
    for (int line = 0; line < UI_LINES; ++line) {
        if (y + GLYPH_HEIGHT > 66)
            break;
        if (!ui->line[line][0])
            continue;
        fill_rect(0, y - 1, WIDTH, GLYPH_HEIGHT + 2, WHITE);
        draw_line(y, ui->line[line]);
        y += LINE_PITCH;
    }
    flush_frame();
}
