#include "assets.h"
#include "tjpgd.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_partition.h"
#include "esp_rom_crc.h"

enum { WIDTH = 128, HEIGHT = 160, EXPR_MAX = 8, FRAME_MAX = 24, ENTRY_MAX = 256, FORMAT_VIDEO = 4,
      JPEG_MAX = 20 * 1024 };

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
    uint16_t hold_ms;
    uint32_t offset;
    uint32_t size;
    uint32_t crc;
} AssetEntry;

_Static_assert(sizeof(AssetHeader) == 32, "asset header");
_Static_assert(sizeof(AssetEntry) == 16, "asset entry");

static const char *TAG = "assets";
static const esp_partition_t *g_part;
static const AssetEntry *g_entries;
static int g_count;
static uint32_t g_version;
static char g_line[96] = "assets missing";
static const uint8_t *g_map;
static esp_partition_mmap_handle_t g_map_handle;
static bool g_ready;
static uint8_t g_jpeg_pool[4096];
static const AssetEntry *g_index[EXPR_MAX][FRAME_MAX];
static uint8_t g_nframes[EXPR_MAX];

typedef struct {
    const uint8_t *data;
    size_t size;
    size_t pos;
    uint16_t *dest;
} JpegClip;

static bool header_sane(const AssetHeader *h, const uint8_t *bytes)
{
    if (memcmp(h->magic, "DFYA", 4) || h->format != FORMAT_VIDEO || h->header_size != sizeof(AssetHeader))
        return false;
    if (h->width != WIDTH || h->height != HEIGHT || h->pixel_format != 2)
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
    memset(g_index, 0, sizeof(g_index));
    memset(g_nframes, 0, sizeof(g_nframes));
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
    if (!memcmp(header.magic, "DFYA", 4) && (header.format == 2 || header.format == 3)) {
        set_line(header.format == 2 ? "assets old zlib pack" : "assets old raw frames");
        ESP_LOGW(TAG, "%s", g_line);
        return;
    }
    if (!header_sane(&header, raw)) {
        set_line("assets bad header");
        ESP_LOGW(TAG, "%s", g_line);
        return;
    }
    // Map only the bytes the pack uses, rounded up to the 64KB cache page.
    size_t need = (size_t)header.header_size + header.data_size;
    if (need > g_part->size) {
        set_line("assets truncated");
        return;
    }
    size_t map_len = (need + 0xffffu) & ~0xffffu;
    if (map_len > g_part->size)
        map_len = g_part->size;
    const void *mapped = 0;
    if (esp_partition_mmap(g_part, 0, map_len, ESP_PARTITION_MMAP_DATA, &mapped, &g_map_handle) != ESP_OK ||
        !mapped) {
        set_line("assets map failed");
        ESP_LOGW(TAG, "%s", g_line);
        return;
    }
    g_map = mapped;
    g_entries = (const AssetEntry *)(g_map + header.header_size);
    memset(g_index, 0, sizeof(g_index));
    memset(g_nframes, 0, sizeof(g_nframes));
    int bad = 0;
    for (unsigned i = 0; i < header.entry_count; ++i) {
        const AssetEntry *e = &g_entries[i];
        if (e->expr >= EXPR_MAX || e->frame >= FRAME_MAX || e->size < 32 || e->size > JPEG_MAX ||
            e->offset < header.header_size || e->offset > need || e->size > need - e->offset ||
            g_index[e->expr][e->frame]) {
            ++bad;
            continue;
        }
        g_index[e->expr][e->frame] = e;
    }
    for (int expr = 0; expr < EXPR_MAX; ++expr) {
        int n = 0;
        while (n < FRAME_MAX && g_index[expr][n])
            ++n;
        for (int frame = n; frame < FRAME_MAX; ++frame)
            if (g_index[expr][frame])
                ++bad; // a hole, so frame % count would show the wrong pose
        g_nframes[expr] = (uint8_t)n;
    }
    g_count = (int)header.entry_count;
    g_version = header.pack_version;
    g_ready = bad == 0;
    int faces = 0;
    for (int expr = 0; expr < EXPR_MAX; ++expr)
        faces += g_nframes[expr] > 0;
    snprintf(g_line, sizeof(g_line), "assets %s v%lu clips %d/8 frames %d mjpeg", g_ready ? "ok" : "partial",
             (unsigned long)g_version, faces, g_count);
    ESP_LOGI(TAG, "%s", g_line);
}

int assets_frames(int expr)
{
    if (expr < 0 || expr >= EXPR_MAX)
        return 0;
    return g_nframes[expr];
}

int assets_hold_ms(int expr, int frame)
{
    if (expr < 0 || expr >= EXPR_MAX || frame < 0 || frame >= FRAME_MAX || !g_index[expr][frame])
        return 450;
    return g_index[expr][frame]->hold_ms ? g_index[expr][frame]->hold_ms : 450;
}

bool assets_has(int expr)
{
    return assets_frames(expr) > 0;
}

int assets_expr_count(void)
{
    int n = 0;
    for (int expr = 0; expr < EXPR_MAX; ++expr)
        n += assets_has(expr);
    return n;
}

const uint8_t *assets_frame(int expr, int frame)
{
    if (!g_ready || !g_map || expr < 0 || expr >= EXPR_MAX || frame < 0 || frame >= FRAME_MAX)
        return 0;
    const AssetEntry *e = g_index[expr][frame];
    if (!e)
        return 0;
    return g_map + e->offset;
}

static size_t jpeg_read(JDEC *jd, uint8_t *buf, size_t len)
{
    JpegClip *clip = jd->device;
    if (clip->pos >= clip->size)
        return 0;
    if (len > clip->size - clip->pos)
        len = clip->size - clip->pos;
    if (buf)
        memcpy(buf, clip->data + clip->pos, len);
    clip->pos += len;
    return len;
}

static int jpeg_write(JDEC *jd, void *bitmap, JRECT *rect)
{
    JpegClip *clip = jd->device;
    if (rect->right >= jd->width || rect->bottom >= jd->height)
        return 0;
    unsigned w = (unsigned)(rect->right - rect->left + 1);
    const uint16_t *src = bitmap;
    for (unsigned y = rect->top; y <= rect->bottom; ++y) {
        memcpy(clip->dest + y * WIDTH + rect->left, src, w * sizeof(uint16_t));
        src += w;
    }
    return 1;
}

bool assets_blit(int expr, int frame, uint16_t *dest)
{
    if (!g_ready || !g_map || !dest || expr < 0 || expr >= EXPR_MAX || frame < 0 || frame >= FRAME_MAX)
        return false;
    const AssetEntry *e = g_index[expr][frame];
    if (!e)
        return false;
    const uint8_t *jpeg = g_map + e->offset;
    if (esp_rom_crc32_le(0, jpeg, e->size) != e->crc)
        return false;
    JpegClip clip = {.data = jpeg, .size = e->size, .pos = 0, .dest = dest};
    JDEC jd;
    memset(&jd, 0, sizeof(jd));
    if (jd_prepare(&jd, jpeg_read, g_jpeg_pool, sizeof(g_jpeg_pool), &clip) != JDR_OK)
        return false;
    // Blank rows above the character are not stored. The frame sits on the
    // bottom of the 160-tall screen; the top is just white.
    if (jd.width != WIDTH || jd.height == 0 || jd.height > HEIGHT)
        return false;
    int y0 = HEIGHT - (int)jd.height;
    for (int i = 0; i < y0 * WIDTH; ++i)
        dest[i] = 0xffff;
    clip.dest = dest + (size_t)y0 * WIDTH;
    return jd_decomp(&jd, jpeg_write, 0) == JDR_OK;
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
    header.format = FORMAT_VIDEO;
    header.header_size = 32;
    header.pack_version = 1;
    header.width = WIDTH;
    header.height = HEIGHT;
    header.pixel_format = 2;
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
    header.format = 3;
    header.header_crc ^= 0x5a;
    memcpy(bytes, &header, 32);
    ASSET_CHECK("bad header crc", !header_sane(&header, bytes));
    const uint8_t known[] = "123456789";
    ASSET_CHECK("crc vector", esp_rom_crc32_le(0, known, 9) == 0xCBF43926u);
    printf("ASSETS SELFTEST %s (%d/%d)\n", pass == total ? "PASS" : "FAIL", pass, total);
    fflush(stdout);
}
