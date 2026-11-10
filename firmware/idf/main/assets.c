#include "assets.h"

#include "lz4.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_partition.h"
#include "esp_rom_crc.h"

enum {
    WIDTH = 128,
    HEIGHT = 160,
    STRIP_MAX = 16,
    STRIP_BYTES = STRIP_MAX * WIDTH * 2,
    ENTRY_MAX = 64,
    HEADER_SIZE = 32,
    PACK_FORMAT = 3,
    CODEC_LZ4_STRIPS = 1
};

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
static esp_partition_mmap_handle_t g_map_handle;
static const uint8_t *g_map;
static size_t g_map_size;
static const AssetEntry *g_entries;
static int g_count;
static uint32_t g_version;
static char g_line[96] = "assets missing";
static bool g_ready;

// The only permanent decode buffer: one 16-line strip (4 KB).
static uint8_t g_strip[STRIP_BYTES];
static int g_cache_expr = -2;
static int g_cache_frame = -1;
static int g_cache_strip = -1;
static int g_cache_h;

extern const uint8_t idle_lz4s_data[];
extern const unsigned idle_lz4s_size;

static bool header_sane(const AssetHeader *h, const uint8_t *bytes)
{
    if (memcmp(h->magic, "DFYA", 4) || h->format != PACK_FORMAT || h->header_size != sizeof(AssetHeader))
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

static int classic_id(int expr)
{
    return (expr < 0 || expr == EXPR_CLASSIC) ? -1 : expr;
}

typedef struct {
    const uint8_t *blob;
    size_t size;
} ImageRef;

static const AssetEntry *find_entry(int expr, int frame)
{
    if (!g_ready || !g_entries || !g_map)
        return 0;
    uint8_t want = classic_id(expr) < 0 ? EXPR_CLASSIC : (uint8_t)expr;
    for (int i = 0; i < g_count; ++i) {
        AssetEntry e;
        memcpy(&e, &g_entries[i], sizeof(e));
        if (e.expr == want && e.frame == frame)
            return &g_entries[i];
    }
    return 0;
}

static bool image_ref(int expr, int frame, ImageRef *out)
{
    const AssetEntry *found = find_entry(expr, frame);
    if (found) {
        AssetEntry e;
        memcpy(&e, found, sizeof(e));
        if (!e.size || e.offset + e.size > g_map_size)
            return false;
        const uint8_t *blob = g_map + e.offset;
        if (esp_rom_crc32_le(0, blob, e.size) != e.crc)
            return false;
        out->blob = blob;
        out->size = e.size;
        return true;
    }
    if (classic_id(expr) < 0) {
        if (idle_lz4s_size < 4)
            return false;
        out->blob = idle_lz4s_data;
        out->size = idle_lz4s_size;
        return true;
    }
    return false;
}

bool assets_src_row(int expr, int frame, int memory_y, uint8_t *dst)
{
    if (!dst || memory_y < 0 || memory_y >= HEIGHT)
        return false;
    ImageRef ref;
    if (!image_ref(expr, frame, &ref) || ref.size < 4)
        return false;
    uint8_t codec = ref.blob[0];
    uint8_t strip_lines = ref.blob[1];
    uint16_t count = 0;
    memcpy(&count, ref.blob + 2, 2);
    if (codec != CODEC_LZ4_STRIPS || strip_lines == 0 || strip_lines > STRIP_MAX || !count)
        return false;
    if (4u + 2u * count > ref.size)
        return false;
    int strip = memory_y / strip_lines;
    if (strip >= (int)count)
        return false;
    int cache_expr = classic_id(expr);
    int y0 = strip * strip_lines;
    int h = strip_lines;
    if (y0 + h > HEIGHT)
        h = HEIGHT - y0;
    int local = memory_y - y0;
    if (cache_expr == g_cache_expr && frame == g_cache_frame && strip == g_cache_strip && local < g_cache_h) {
        memcpy(dst, g_strip + local * WIDTH * 2, WIDTH * 2);
        return true;
    }
    uint16_t sz = 0;
    size_t at = 4u + 2u * count;
    for (int i = 0; i <= strip; ++i) {
        memcpy(&sz, ref.blob + 4 + 2 * i, 2);
        if (i < strip)
            at += sz;
    }
    if (!sz || at + sz > ref.size)
        return false;
    int want = h * WIDTH * 2;
    if (want <= 0 || want > STRIP_BYTES)
        return false;
    int n = LZ4_decompress_safe((const char *)(ref.blob + at), (char *)g_strip, (int)sz, want);
    if (n != want) {
        g_cache_strip = -1;
        return false;
    }
    g_cache_expr = cache_expr;
    g_cache_frame = frame;
    g_cache_strip = strip;
    g_cache_h = h;
    memcpy(dst, g_strip + local * WIDTH * 2, WIDTH * 2);
    return true;
}

void assets_init(void)
{
    g_ready = false;
    g_count = 0;
    g_entries = 0;
    g_map = 0;
    g_map_size = 0;
    g_cache_expr = -2;
    g_cache_strip = -1;
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
    if (!header_sane(&header, raw)) {
        set_line("assets bad header");
        ESP_LOGW(TAG, "%s", g_line);
        return;
    }
    size_t used = (size_t)header.header_size + header.data_size;
    if (used < HEADER_SIZE || used > g_part->size) {
        set_line("assets bad size");
        return;
    }
    const void *ptr = 0;
    if (esp_partition_mmap(g_part, 0, used, ESP_PARTITION_MMAP_DATA, &ptr, &g_map_handle) != ESP_OK) {
        set_line("assets mmap failed");
        ESP_LOGW(TAG, "%s", g_line);
        return;
    }
    g_map = (const uint8_t *)ptr;
    g_map_size = used;
    if (esp_rom_crc32_le(0, g_map + header.header_size, header.data_size) != header.data_crc) {
        set_line("assets bad crc");
        ESP_LOGW(TAG, "%s", g_line);
        esp_partition_munmap(g_map_handle);
        g_map = 0;
        g_map_size = 0;
        return;
    }
    g_entries = (const AssetEntry *)(g_map + header.header_size);
    g_count = (int)header.entry_count;
    g_version = header.pack_version;
    int bad = 0;
    for (int i = 0; i < g_count; ++i) {
        AssetEntry e;
        memcpy(&e, &g_entries[i], sizeof(e));
        if ((e.expr >= 8 && e.expr != EXPR_CLASSIC) || e.size == 0 || e.offset + e.size > g_map_size)
            ++bad;
    }
    g_ready = true;
    int faces = 0;
    for (int expr = 0; expr < 8; ++expr)
        faces += assets_has(expr);
    snprintf(g_line, sizeof(g_line), "assets %s v%lu faces %d/8 frames %d bad %d", bad ? "partial" : "ok",
             (unsigned long)g_version, faces, g_count, bad);
    ESP_LOGI(TAG, "%s", g_line);
}

int assets_frames(int expr)
{
    if (classic_id(expr) < 0)
        return 1;
    if (!g_entries)
        return 0;
    int n = 0;
    for (int i = 0; i < g_count; ++i) {
        AssetEntry e;
        memcpy(&e, &g_entries[i], sizeof(e));
        if (e.expr == expr)
            ++n;
    }
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

const char *assets_status_line(void)
{
    return g_line;
}

void assets_print(void)
{
    printf("%s\n", g_line);
    for (int i = 0; i < g_count; ++i) {
        AssetEntry e;
        memcpy(&e, &g_entries[i], sizeof(e));
        printf("  expr %u frame %u %lu bytes\n", e.expr, e.frame, (unsigned long)e.size);
    }
    fflush(stdout);
}

void assets_selftest(void)
{
    int pass = 0, total = 0;
    uint8_t bytes[32];
    AssetHeader header;
    memset(&header, 0, sizeof(header));
    memcpy(header.magic, "DFYA", 4);
    header.format = PACK_FORMAT;
    header.header_size = 32;
    header.pack_version = 101;
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
        printf("ASSETS SELFTEST %-22s %s\n", name, ok_ ? "PASS" : "FAIL");                                   \
    } while (0)
    ASSET_CHECK("good header", header_sane(&header, bytes));
    bytes[0] ^= 0x01;
    ASSET_CHECK("flipped magic", !header_sane(&header, bytes));
    bytes[0] ^= 0x01;
    header.format = 2;
    memcpy(bytes, &header, 32);
    ASSET_CHECK("old format", !header_sane(&header, bytes));
    header.format = PACK_FORMAT;
    header.header_crc ^= 0x5a;
    memcpy(bytes, &header, 32);
    ASSET_CHECK("bad header crc", !header_sane(&header, bytes));
    const uint8_t known[] = "123456789";
    ASSET_CHECK("crc vector", esp_rom_crc32_le(0, known, 9) == 0xCBF43926u);

    uint32_t crc = 0;
    uint8_t row[WIDTH * 2];
    bool decoded = true;
    // Force the built-in fallback path even if a pack is mapped.
    bool was_ready = g_ready;
    const AssetEntry *saved_entries = g_entries;
    g_ready = false;
    g_entries = 0;
    g_cache_strip = -1;
    for (int y = 0; y < STRIP_MAX; ++y) {
        if (!assets_src_row(-1, 0, y, row)) {
            decoded = false;
            break;
        }
        crc = esp_rom_crc32_le(crc, row, sizeof(row));
    }
    g_ready = was_ready;
    g_entries = saved_entries;
    g_cache_strip = -1;
    ASSET_CHECK("classic first strip", decoded && crc == 0x483711bbu);
    printf("ASSETS SELFTEST %s (%d/%d)\n", pass == total ? "PASS" : "FAIL", pass, total);
    fflush(stdout);
}
