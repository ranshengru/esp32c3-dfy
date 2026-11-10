#include "assets.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_partition.h"
#include "esp_rom_crc.h"

enum { WIDTH = 128, HEIGHT = 160, RAW = WIDTH * HEIGHT * 2, ENTRY_MAX = 64, FORMAT_RAW = 3 };

typedef struct __attribute__((packed)) {
    char magic[4];
    uint16_t format;
    uint16_t header_size;
    uint32_t pack_version;
    uint16_t width;
    uint16_t height;
    uint8_t pixel_format;
    uint8_t flags;
    uint16_t entry_count;
    uint32_t data_size;
    uint32_t data_crc;
    uint32_t header_crc;
} AssetHeader;

typedef struct __attribute__((packed)) {
    uint8_t expr;
    uint8_t frame;
    uint16_t reserved;
    uint32_t offset;
    uint32_t size;
    uint32_t crc;
} AssetEntry;

_Static_assert(sizeof(AssetHeader) == 32, "asset header");
_Static_assert(sizeof(AssetEntry) == 16, "asset entry");

static const char *TAG = "assets";
static const esp_partition_t *g_part;
static AssetEntry g_entries[ENTRY_MAX];
static int g_count;
static uint32_t g_version;
static char g_line[96] = "assets missing";
static const uint8_t *g_map;
static esp_partition_mmap_handle_t g_map_handle;
static bool g_ready;

static bool header_sane(const AssetHeader *h, const uint8_t *bytes)
{
    if (memcmp(h->magic, "DFYA", 4) || h->format != FORMAT_RAW || h->header_size != sizeof(AssetHeader))
        return false;
    if (h->width != WIDTH || h->height != HEIGHT || h->pixel_format != 1)
        return false;
    if (!h->entry_count || h->entry_count > ENTRY_MAX)
        return false;
    return esp_rom_crc32_le(0, bytes, 28) == h->header_crc;
}

static void set_line(const char *text)
{
    snprintf(g_line, sizeof(g_line), "%s", text);
}

void assets_init(void)
{
    g_ready = false;
    g_count = 0;
    g_map = 0;
    g_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0x40, "assets");
    if (!g_part) {
        set_line("assets missing");
        ESP_LOGW(TAG, "%s", g_line);
        return;
    }
    uint8_t raw[sizeof(AssetHeader)];
    AssetHeader header;
    if (esp_partition_read(g_part, 0, raw, sizeof(raw)) != ESP_OK) {
        set_line("assets unreadable");
        return;
    }
    memcpy(&header, raw, sizeof(header));
    if (!memcmp(header.magic, "DFYA", 4) && header.format == 2) {
        set_line("assets old zlib pack");
        ESP_LOGW(TAG, "%s", g_line);
        return;
    }
    if (!header_sane(&header, raw)) {
        set_line("assets bad header");
        ESP_LOGW(TAG, "%s", g_line);
        return;
    }
    // Whole partition is 64KB-aligned, so the flash cache can map it. Pixels
    // stay in flash; nothing is copied into RAM.
    const void *mapped = 0;
    if (esp_partition_mmap(g_part, 0, g_part->size, ESP_PARTITION_MMAP_DATA, &mapped, &g_map_handle) != ESP_OK ||
        !mapped) {
        set_line("assets map failed");
        ESP_LOGW(TAG, "%s", g_line);
        return;
    }
    g_map = mapped;
    if (header.data_size > g_part->size - header.header_size ||
        esp_rom_crc32_le(0, g_map + header.header_size, header.data_size) != header.data_crc) {
        set_line("assets bad crc");
        ESP_LOGW(TAG, "%s", g_line);
        return;
    }
    memcpy(g_entries, g_map + header.header_size, header.entry_count * sizeof(AssetEntry));
    int bad = 0;
    for (unsigned i = 0; i < header.entry_count; ++i) {
        const AssetEntry *e = &g_entries[i];
        if (e->expr >= 8 || e->size != RAW || e->offset + e->size > g_part->size)
            ++bad;
    }
    g_count = (int)header.entry_count;
    g_version = header.pack_version;
    g_ready = bad == 0;
    int faces = 0;
    for (int expr = 0; expr < 8; ++expr)
        faces += assets_has(expr);
    snprintf(g_line, sizeof(g_line), "assets %s v%lu faces %d/8 frames %d flash", g_ready ? "ok" : "partial",
             (unsigned long)g_version, faces, g_count);
    ESP_LOGI(TAG, "%s", g_line);
}

int assets_frames(int expr)
{
    int n = 0;
    for (int i = 0; i < g_count; ++i)
        if (g_entries[i].expr == expr)
            ++n;
    return n;
}

bool assets_has(int expr)
{
    return assets_frames(expr) > 0;
}

int assets_expr_count(void)
{
    int n = 0;
    for (int expr = 0; expr < 8; ++expr)
        n += assets_has(expr);
    return n;
}

const uint8_t *assets_frame(int expr, int frame)
{
    if (!g_ready || !g_map || expr < 0)
        return 0;
    for (int i = 0; i < g_count; ++i) {
        const AssetEntry *e = &g_entries[i];
        if (e->expr == expr && e->frame == frame && e->size == RAW && e->offset + RAW <= g_part->size)
            return g_map + e->offset;
    }
    return 0;
}

const char *assets_status_line(void)
{
    return g_line;
}

void assets_print(void)
{
    printf("%s\n", g_line);
    for (int i = 0; i < g_count; ++i)
        printf("  expr %u frame %u %lu bytes\n", g_entries[i].expr, g_entries[i].frame,
               (unsigned long)g_entries[i].size);
    fflush(stdout);
}

void assets_selftest(void)
{
    int pass = 0, total = 0;
    uint8_t bytes[32];
    AssetHeader header;
    memset(&header, 0, sizeof(header));
    memcpy(header.magic, "DFYA", 4);
    header.format = FORMAT_RAW;
    header.header_size = 32;
    header.pack_version = 1;
    header.width = WIDTH;
    header.height = HEIGHT;
    header.pixel_format = 1;
    header.entry_count = 1;
    header.data_size = 16;
    header.data_crc = 0;
    memcpy(bytes, &header, 28);
    header.header_crc = esp_rom_crc32_le(0, bytes, 28);
    memcpy(bytes, &header, 32);
#define ASSET_CHECK(name, cond)                                                                              \
    do {                                                                                                     \
        bool ok_ = (cond);                                                                                   \
        ++total;                                                                                             \
        pass += ok_;                                                                                         \
        printf("ASSETS SELFTEST %-18s %s\n", name, ok_ ? "PASS" : "FAIL");                                   \
    } while (0)
    ASSET_CHECK("good header", header_sane(&header, bytes));
    bytes[0] ^= 0x01;
    ASSET_CHECK("flipped magic", !header_sane(&header, bytes));
    bytes[0] ^= 0x01;
    header.format = 2;
    memcpy(bytes, &header, 32);
    ASSET_CHECK("zlib format", !header_sane(&header, bytes));
    header.format = FORMAT_RAW;
    header.header_crc ^= 0x5a;
    memcpy(bytes, &header, 32);
    ASSET_CHECK("bad header crc", !header_sane(&header, bytes));
    const uint8_t known[] = "123456789";
    ASSET_CHECK("crc vector", esp_rom_crc32_le(0, known, 9) == 0xCBF43926u);
    printf("ASSETS SELFTEST %s (%d/%d)\n", pass == total ? "PASS" : "FAIL", pass, total);
    fflush(stdout);
}
