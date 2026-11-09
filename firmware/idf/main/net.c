// Screen lines are truncated on purpose; the panel only fits about 7 hanzi.
#pragma GCC diagnostic ignored "-Wformat-truncation"
#include "net.h"
#include "portal.h"
#include "prompts.h"
#include "quotes.h"
#include "sonar.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_rom_crc.h"
#include "esp_sntp.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "mdns.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "net";
static const char *NVS_NS = "pet";
static const char *BALANCE_URL = "https://api.deepseek.com/user/balance";
// Cheapest DeepSeek model from GET /models (2026-11): "deepseek-flash" (DeepSeek-V4.1-Flash).
// Thinking is disabled so the 24-token budget goes to the answer.
#define LLM_MODEL "deepseek-flash"
enum { LLM_DAILY_CAP = 4 };
static const int64_t AP_FAIL_US = 3LL * 60 * 1000000LL;      // STA down this long -> setup hotspot
static const int64_t AP_LIFETIME_US = 10LL * 60 * 1000000LL; // hotspot auto-off when nobody joined
static const int64_t AP_REARM_US = 5LL * 60 * 1000000LL;     // pause before the hotspot may come back

static SemaphoreHandle_t mu;
static TaskHandle_t net_task_handle;

static char g_ssid[32];
static char g_pass[64];
static char g_key[128];
static char g_ip[20];
static char g_wifi[24];
static char g_detail[40];
static char g_total[16];
static char g_granted[16];
static char g_topped[16];
static char g_currency[8];
static bool g_connected;
static bool g_scan_in_progress;
static bool g_available;
static bool g_fetch_pending;
static int64_t g_last_fetch_us;
static int64_t g_retry_at_us;
static char g_city[32];
static char g_weather[24];
static char g_phrase[96];
static char g_remind[40];
static int g_lat_e6;
static int g_lon_e6;
static bool g_has_geo;
static int g_drink_min = 45;
static int g_move_min = 60;
static int g_quiet_start = 23;
static int g_quiet_end = 8;
static int g_tz_hours = 8;
static int g_drinks_today;
static int g_drink_day = -1;
static int g_remind_kind;
static int64_t g_phrase_until_us;
// g_phrase markers: show the balance card instead of a bubble.
#define PHRASE_BALANCE "\001bal"
#define PHRASE_BALANCE_WAIT "\001wait"
static int64_t g_remind_until_us;
static int64_t g_last_drink_us;
static int64_t g_last_move_us;
static int64_t g_next_weather_us;
static int64_t g_weather_ok_us; // last good Open-Meteo fetch (0 = none yet)
#define WEATHER_EVERY_US (30LL * 60 * 1000000LL)
#define WEATHER_RETRY_US (2LL * 60 * 1000000LL)
static int64_t g_arm_us;
static bool g_arm_double;
static bool g_refill_pending;
static wifi_ap_record_t g_scan_records[24];

// v0.5 state
static bool g_geo_fixed;          // coordinates typed by the user, skip geocoding
static int g_mode;                // 0 companion, 1 birthday
static char g_bname[32];
static int g_bmonth;
static int g_bday;
static bool g_bauto = true;
static char g_msg[40];
static bool g_rpause;
static int g_llm_day = -1;
static int g_llm_count;
static int64_t g_ip_until_us;
static char g_wx_today[24];
static char g_wx_tomorrow[24];
static bool g_ap_active;
static bool g_ap_want_off;
static char g_ap_ssid[24];
static char g_ap_pass[12];
static int g_ap_clients;
static int64_t g_ap_until_us;
static int64_t g_ap_next_ok_us;
static int64_t g_sta_down_since_us;
static bool g_ap_request;
static bool g_ap_stop_request;
static uint32_t g_anim_seq;
static int g_wcode = -1;
static int g_temp_c;
static bool g_balance_bubble;
static char g_bal_hhmm[8]; // local time of the last good balance query

static uint16_t g_wish_used; // shuffle bag over BIRTHDAY_WISHES
static uint32_t g_fx_seq;      // fireworks
static uint32_t g_blow_seq;    // blow out candles
static int64_t g_candles_out_until_us;
static int64_t g_last_fx_us;
static bool g_was_party;
static int64_t g_demo_until_us; // BDAYDEMO: birthday visuals without the date
static int g_wish_last = -1;
static const char *const WEEKDAYS[] = {"周日", "周一", "周二", "周三", "周四", "周五", "周六"};

static void label(const char *text)
{
    snprintf(g_wifi, sizeof(g_wifi), "%s", text);
}

static void detail(const char *text)
{
    snprintf(g_detail, sizeof(g_detail), "%s", text);
}

static esp_err_t nvs_put(const char *key, const char *value)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &handle);
    if (err != ESP_OK)
        return err;
    err = nvs_set_str(handle, key, value);
    if (err == ESP_OK)
        err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

static void nvs_load(const char *key, char *out, size_t out_len)
{
    nvs_handle_t handle;
    if (nvs_open(NVS_NS, NVS_READONLY, &handle) != ESP_OK)
        return;
    size_t length = out_len;
    if (nvs_get_str(handle, key, out, &length) != ESP_OK)
        out[0] = 0;
    nvs_close(handle);
}

static int load_int(const char *key, int fallback, int min, int max)
{
    char text[8] = {0};
    nvs_load(key, text, sizeof(text));
    if (!text[0])
        return fallback;
    int value = atoi(text);
    if (value < min || value > max)
        return fallback;
    return value;
}

static void save_int(const char *key, int value)
{
    char text[8];
    snprintf(text, sizeof(text), "%d", value);
    nvs_put(key, text);
}

static void apply_tz(void)
{
    setenv("TZ", g_tz_hours == 9 ? "JST-9" : "CST-8", 1);
    tzset();
}

static void queue_weather(void)
{
    g_next_weather_us = 0;
    if (net_task_handle)
        xTaskNotifyGive(net_task_handle);
}

static void fill_sta_config(wifi_config_t *cfg, const char *ssid, const char *pass)
{
    memset(cfg, 0, sizeof(*cfg));
    memcpy(cfg->sta.ssid, ssid, strnlen(ssid, sizeof(cfg->sta.ssid)));
    memcpy(cfg->sta.password, pass, strnlen(pass, sizeof(cfg->sta.password)));
    cfg->sta.threshold.authmode = pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    cfg->sta.pmf_cfg.capable = true;
    cfg->sta.pmf_cfg.required = false;
}

// Copies credentials, then calls the Wi-Fi driver without holding mu.
// The event handler also takes mu, and the driver can post events inline.
static void start_connect(void)
{
    char ssid[sizeof(g_ssid)];
    char pass[sizeof(g_pass)];
    xSemaphoreTake(mu, portMAX_DELAY);
    memcpy(ssid, g_ssid, sizeof(ssid));
    memcpy(pass, g_pass, sizeof(pass));
    xSemaphoreGive(mu);
    if (!ssid[0]) {
        esp_wifi_disconnect();
        return;
    }
    if (pass[0] && strlen(pass) < 8) {
        xSemaphoreTake(mu, portMAX_DELAY);
        label("密码错");
        detail("至少8位");
        xSemaphoreGive(mu);
        return;
    }
    wifi_config_t cfg;
    fill_sta_config(&cfg, ssid, pass);
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "wifi config %s", esp_err_to_name(err));
        xSemaphoreTake(mu, portMAX_DELAY);
        detail("配置失败");
        xSemaphoreGive(mu);
        return;
    }
    esp_wifi_disconnect();
    esp_wifi_connect();
}

static void request_reconnect(void)
{
    xSemaphoreTake(mu, portMAX_DELAY);
    g_connected = false;
    g_ip[0] = 0;
    g_retry_at_us = 0;
    if (!g_ssid[0])
        label("未设置");
    else
        label("连接中");
    xSemaphoreGive(mu);
    start_connect();
}

static bool queue_fetch(int64_t min_gap_us)
{
    int64_t now = esp_timer_get_time();
    if (g_last_fetch_us && now - g_last_fetch_us < min_gap_us)
        return false;
    g_last_fetch_us = now;
    g_fetch_pending = true;
    if (net_task_handle)
        xTaskNotifyGive(net_task_handle);
    return true;
}

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    bool kick = false;
    xSemaphoreTake(mu, portMAX_DELAY);
    if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED) {
        ++g_ap_clients;
        ESP_LOGI(TAG, "setup hotspot client joined (%d)", g_ap_clients);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STADISCONNECTED) {
        if (g_ap_clients > 0)
            --g_ap_clients;
        ESP_LOGI(TAG, "setup hotspot client left (%d)", g_ap_clients);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (g_ssid[0]) {
            label("连接中");
            kick = true;
        } else {
            label("未设置");
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *event = data;
        g_connected = false;
        g_ip[0] = 0;
        if (!g_sta_down_since_us)
            g_sta_down_since_us = esp_timer_get_time();
        int64_t delay_us = 0;
        if (event->reason == WIFI_REASON_ASSOC_LEAVE) {
            ESP_LOGI(TAG, "wifi left");
        } else if (event->reason == WIFI_REASON_NO_AP_FOUND) {
            label("找不到");
            delay_us = 4000000;
        } else if (event->reason == WIFI_REASON_AUTH_FAIL ||
                   event->reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT ||
                   event->reason == WIFI_REASON_HANDSHAKE_TIMEOUT) {
            label("密码错");
            delay_us = 8000000;
        } else {
            label("已断开");
            delay_us = 2000000;
        }
        if (delay_us)
            g_retry_at_us = esp_timer_get_time() + delay_us;
        ESP_LOGW(TAG, "wifi disconnect reason %u", event->reason);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = data;
        g_connected = true;
        g_retry_at_us = 0;
        snprintf(g_ip, sizeof(g_ip), IPSTR, IP2STR(&event->ip_info.ip));
        label("已连接");
        g_sta_down_since_us = 0;
        g_ip_until_us = esp_timer_get_time() + 30 * 1000000LL;
        if (g_ap_active)
            g_ap_want_off = true; // keep it a minute so the phone sees the result
        if (g_ap_active)
            g_ap_until_us = esp_timer_get_time() + 60 * 1000000LL;
        g_fetch_pending = true;
        g_last_fetch_us = esp_timer_get_time();
        g_next_weather_us = 0;
        ESP_LOGI(TAG, "wifi ip %s", g_ip);
        if (net_task_handle)
            xTaskNotifyGive(net_task_handle);
    }
    xSemaphoreGive(mu);
    if (kick)
        start_connect();
}

static bool time_ready(void)
{
    time_t now = time(NULL);
    return now > 1700000000;
}

static void ensure_sntp(void)
{
    static bool started;
    if (started)
        return;
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "ntp.aliyun.com");
    esp_sntp_setservername(1, "time.cloudflare.com");
    esp_sntp_setservername(2, "pool.ntp.org");
    esp_sntp_init();
    started = true;
}

static char body[6144];
static int body_len;

static esp_err_t http_event(esp_http_client_event_t *event)
{
    if (event->event_id == HTTP_EVENT_ON_DATA && event->data_len > 0) {
        int room = (int)sizeof(body) - 1 - body_len;
        int copy = event->data_len < room ? event->data_len : room;
        if (copy > 0) {
            memcpy(body + body_len, event->data, (size_t)copy);
            body_len += copy;
            body[body_len] = 0;
        }
    }
    return ESP_OK;
}

static int https_call(const char *url, const char *bearer, const char *json_body)
{
    body_len = 0;
    body[0] = 0;
    esp_http_client_config_t config = {
        .url = url,
        .method = json_body ? HTTP_METHOD_POST : HTTP_METHOD_GET,
        .timeout_ms = 12000,
        .event_handler = http_event,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 1024,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client)
        return -1;
    if (bearer) {
        char auth[180];
        snprintf(auth, sizeof(auth), "Bearer %s", bearer);
        esp_http_client_set_header(client, "Authorization", auth);
    }
    esp_http_client_set_header(client, "Accept", "application/json");
    if (json_body) {
        esp_http_client_set_header(client, "Content-Type", "application/json");
        esp_http_client_set_post_field(client, json_body, (int)strlen(json_body));
    }
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "http %s", esp_err_to_name(err));
        return -1;
    }
    return status;
}

static bool utf8_next(const unsigned char **text, uint32_t *codepoint)
{
    const unsigned char *p = *text;
    if (!*p)
        return false;
    if (*p < 0x80) {
        *codepoint = *p;
        *text = p + 1;
        return true;
    }
    if ((*p & 0xe0) == 0xc0 && (p[1] & 0xc0) == 0x80) {
        *codepoint = ((*p & 0x1f) << 6) | (p[1] & 0x3f);
        *text = p + 2;
        return true;
    }
    if ((*p & 0xf0) == 0xe0 && (p[1] & 0xc0) == 0x80 && (p[2] & 0xc0) == 0x80) {
        *codepoint = ((*p & 0x0f) << 12) | ((p[1] & 0x3f) << 6) | (p[2] & 0x3f);
        *text = p + 3;
        return true;
    }
    return false;
}

// ---- sentence pool ---------------------------------------------------------
// 20 built-in seed lines (quotes.h) plus up to POOL_GEN_MAX lines generated in
// rare DeepSeek batches. Every line has a bucket (time of day x condition);
// at display time only lines matching the current time and weather are
// eligible (context_now_locked), and lines made for exactly this moment are
// weighted up. Shuffle bag: a line is not repeated until every eligible line
// has been shown once this round, and never one of the last POOL_RECENT lines.
// Lines shown POOL_RETIRE times retire (generated ones are deleted to make
// room). When fewer than 20% of active lines are unshown this round, one batch
// refill is queued (still bound by the 12 h spacing and the 4-calls-a-day cap
// in llm_take_slot). Everything persists in one CRC-checked NVS blob; a blob
// that fails the size/version/CRC check, or any single line that fails
// validation, is dropped and the built-in seeds carry on.

enum {
    POOL_GEN_MAX = 40,
    POOL_TEXT = 72,
    POOL_RETIRE = 5,
    POOL_RECENT = 6,
    POOL_LOW_PCT = 20,
    POOL_BATCH = 20,
    POOL_RETIRED = 255,
    POOL_VERSION = 2,
};

#define FILL_TEMP_WORST "-10°C"
#define FILL_WEATHER_WORST "雷阵雨"

typedef struct {
    uint8_t tag; // time bucket << 4 | condition (quotes.h T_* / C_*)
    uint8_t shows;
    uint8_t used;
    uint8_t pad;
    char text[POOL_TEXT];
} GenLine;

typedef struct {
    uint8_t version;
    uint8_t builtin_count;
    uint16_t refills;
    uint32_t seed_hash;
    uint8_t bshows[PET_QUOTE_COUNT];
    uint8_t bused[PET_QUOTE_COUNT];
    GenLine gen[POOL_GEN_MAX];
    uint32_t crc; // CRC32 of every byte before this field
} PoolStore;

// v0.5 pre-release layout (single tag per line), migrated on load.
typedef struct {
    uint8_t version;
    uint8_t builtin_count;
    uint8_t bshows[20];
    uint8_t bused[20];
    GenLine gen[POOL_GEN_MAX];
} PoolStoreV1;

static PoolStore g_pool;
static int g_boots;
static const char *g_reset_name = "?";
static int g_recent[POOL_RECENT] = {-1, -1, -1, -1, -1, -1};
static int g_recent_pos;
static bool g_pool_dirty;
static int64_t g_pool_saved_us;
static bool g_refill_force;
static char g_pool_boot[48] = "-";

static const char *weather_name(int code);
static bool pool_line_ok(char *text);

static void utf8_put(char *out, int *used, int size, uint32_t c)
{
    int bytes = c < 0x80 ? 1 : c < 0x800 ? 2 : 3;
    if (*used + bytes >= size)
        return;
    if (c < 0x80) {
        out[(*used)++] = (char)c;
    } else if (c < 0x800) {
        out[(*used)++] = (char)(0xc0 | (c >> 6));
        out[(*used)++] = (char)(0x80 | (c & 0x3f));
    } else {
        out[(*used)++] = (char)(0xe0 | (c >> 12));
        out[(*used)++] = (char)(0x80 | ((c >> 6) & 0x3f));
        out[(*used)++] = (char)(0x80 | (c & 0x3f));
    }
}

static uint32_t seed_hash(void)
{
    uint32_t h = 2166136261u;
    for (int i = 0; i < PET_QUOTE_COUNT; ++i) {
        h = (h ^ PET_QUOTES[i].time) * 16777619u;
        h = (h ^ PET_QUOTES[i].cond) * 16777619u;
        for (const char *p = PET_QUOTES[i].text; *p; ++p)
            h = (h ^ (uint8_t)*p) * 16777619u;
    }
    return h;
}

static uint32_t pool_crc(const PoolStore *p)
{
    return esp_rom_crc32_le(0, (const uint8_t *)p, offsetof(PoolStore, crc));
}

// Replaces {temp} / {weather}; false if a placeholder is unknown.
static bool fill_line(char *out, size_t size, const char *text, const char *temp, const char *weather)
{
    size_t used = 0;
    out[0] = 0;
    while (*text) {
        const char *put = 0;
        size_t skip = 1;
        if (!strncmp(text, "{temp}", 6)) {
            put = temp;
            skip = 6;
        } else if (!strncmp(text, "{weather}", 9)) {
            put = weather;
            skip = 9;
        } else if (*text == '{' || *text == '}') {
            return false;
        }
        if (put) {
            size_t n = strlen(put);
            if (used + n >= size)
                return false;
            memcpy(out + used, put, n);
            used += n;
        } else {
            if (used + 1 >= size)
                return false;
            out[used++] = *text;
        }
        text += skip;
    }
    out[used] = 0;
    return true;
}

static bool has_placeholder(const char *text)
{
    return strstr(text, "{temp}") || strstr(text, "{weather}");
}

// Checks every field of a loaded blob; returns how many lines were dropped.
static int pool_validate(PoolStore *p)
{
    int dropped = 0;
    for (int i = 0; i < PET_QUOTE_COUNT; ++i) {
        if (p->bshows[i] > POOL_RETIRE && p->bshows[i] != POOL_RETIRED)
            p->bshows[i] = 0;
        if (p->bused[i] > 1)
            p->bused[i] = 0;
    }
    for (int i = 0; i < POOL_GEN_MAX; ++i) {
        GenLine *g = &p->gen[i];
        if (!g->text[0]) {
            memset(g, 0, sizeof(*g));
            continue;
        }
        bool ok = memchr(g->text, 0, POOL_TEXT) != 0 && (g->tag >> 4) < T_COUNT && (g->tag & 15) < C_COUNT &&
                  g->shows < POOL_RETIRE && g->used <= 1;
        if (ok) {
            char copy[POOL_TEXT];
            strcpy(copy, g->text);
            ok = pool_line_ok(copy) && !strcmp(copy, g->text);
        }
        if (!ok) {
            memset(g, 0, sizeof(*g));
            ++dropped;
        }
    }
    return dropped;
}

// Decodes a raw NVS blob into *out (always leaves a usable pool) and writes a
// short status ("ok", "new", "migrated v1", "corrupt crc"...) into status.
static void pool_decode(const void *blob, size_t length, PoolStore *out, char *status, size_t status_len)
{
    memset(out, 0, sizeof(*out));
    int dropped = 0;
    if (!blob || !length) {
        snprintf(status, status_len, "new");
    } else if (length == sizeof(PoolStore) && ((const PoolStore *)blob)->version == POOL_VERSION) {
        memcpy(out, blob, sizeof(*out));
        if (pool_crc(out) != out->crc) {
            memset(out, 0, sizeof(*out));
            snprintf(status, status_len, "corrupt crc, seeds only");
        } else {
            dropped = pool_validate(out);
            snprintf(status, status_len, "ok");
        }
    } else if (length == sizeof(PoolStoreV1) && ((const PoolStoreV1 *)blob)->version == 1) {
        static const uint8_t map[8] = {T_ANY << 4 | C_GENERAL,     T_MORNING << 4 | C_GENERAL,
                                       T_NOON << 4 | C_GENERAL,    T_EVENING << 4 | C_GENERAL,
                                       T_LATE << 4 | C_GENERAL,    T_ANY << 4 | C_RAIN,
                                       T_ANY << 4 | C_HOT,         T_ANY << 4 | C_COLD};
        const PoolStoreV1 *old = blob;
        memcpy(out->gen, old->gen, sizeof(out->gen));
        for (int i = 0; i < POOL_GEN_MAX; ++i)
            out->gen[i].tag = out->gen[i].tag < 8 ? map[out->gen[i].tag] : 0xff;
        dropped = pool_validate(out);
        snprintf(status, status_len, "migrated v1");
    } else {
        snprintf(status, status_len, "corrupt size %u, seeds only", (unsigned)length);
    }
    if (out->builtin_count != PET_QUOTE_COUNT || out->seed_hash != seed_hash()) {
        // Seed list changed in firmware: restart its counters, keep generated lines.
        memset(out->bshows, 0, sizeof(out->bshows));
        memset(out->bused, 0, sizeof(out->bused));
    }
    out->version = POOL_VERSION;
    out->builtin_count = PET_QUOTE_COUNT;
    out->seed_hash = seed_hash();
    if (dropped) {
        size_t n = strlen(status);
        snprintf(status + n, status_len - n, ", dropped %d bad line(s)", dropped);
    }
}

// Reads blob `key` from NVS and decodes it.
static void pool_read_key(const char *key, PoolStore *out, char *status, size_t status_len)
{
    void *raw = 0;
    size_t length = 0;
    bool bad = false;
    nvs_handle_t handle;
    if (nvs_open(NVS_NS, NVS_READONLY, &handle) == ESP_OK) {
        if (nvs_get_blob(handle, key, 0, &length) == ESP_OK && length) {
            raw = length <= sizeof(PoolStore) + 64 ? malloc(length) : 0;
            if (!raw || nvs_get_blob(handle, key, raw, &length) != ESP_OK) {
                free(raw);
                raw = 0;
                bad = true;
            }
        } else {
            length = 0;
        }
        nvs_close(handle);
    }
    pool_decode(raw, raw ? length : 0, out, status, status_len);
    if (bad)
        snprintf(status, status_len, "unreadable (%u bytes), seeds only", (unsigned)length);
    free(raw);
}

static void pool_save(void);

static void pool_load(void)
{
    pool_read_key("pool", &g_pool, g_pool_boot, sizeof(g_pool_boot));
    printf("POOL boot: %s\n", g_pool_boot);
    if (strcmp(g_pool_boot, "ok"))
        pool_save(); // store the repaired / migrated / fresh pool right away
}

static void pool_save(void)
{
    nvs_handle_t handle;
    if (nvs_open(NVS_NS, NVS_READWRITE, &handle) != ESP_OK)
        return;
    xSemaphoreTake(mu, portMAX_DELAY);
    g_pool.crc = pool_crc(&g_pool);
    esp_err_t err = nvs_set_blob(handle, "pool", &g_pool, sizeof(g_pool));
    g_pool_dirty = false;
    xSemaphoreGive(mu);
    if (err == ESP_OK)
        err = nvs_commit(handle);
    nvs_close(handle);
    if (err != ESP_OK)
        printf("POOL save failed: %s\n", esp_err_to_name(err));
    g_pool_saved_us = esp_timer_get_time();
}

static int pool_gen_count(const PoolStore *p)
{
    int n = 0;
    for (int i = 0; i < POOL_GEN_MAX; ++i)
        n += p->gen[i].text[0] != 0;
    return n;
}

static bool pool_write_key(const char *key, const void *data, size_t length)
{
    nvs_handle_t handle;
    if (nvs_open(NVS_NS, NVS_READWRITE, &handle) != ESP_OK)
        return false;
    esp_err_t err = nvs_set_blob(handle, key, data, length);
    if (err == ESP_OK)
        err = nvs_commit(handle);
    nvs_close(handle);
    return err == ESP_OK;
}

// POOL SELFTEST: runs the boot loader on deliberately damaged copies of the
// pool written to a scratch NVS key ("pooltst"); the real "pool" is untouched.
static void pool_selftest(void)
{
    PoolStore *a = malloc(sizeof(PoolStore));
    PoolStore *b = malloc(sizeof(PoolStore));
    if (!a || !b) {
        free(a);
        free(b);
        printf("POOL SELFTEST no memory\n");
        return;
    }
    char st[64];
    int pass = 0, total = 0;
    xSemaphoreTake(mu, portMAX_DELAY);
    memcpy(a, &g_pool, sizeof(*a));
    xSemaphoreGive(mu);
    a->crc = pool_crc(a);
    int gen0 = pool_gen_count(a);
#define POOL_CHECK(name, cond)                                                                       \
    do {                                                                                             \
        bool ok_ = (cond);                                                                           \
        ++total;                                                                                     \
        pass += ok_;                                                                                 \
        printf("POOL SELFTEST %-22s %s -> %s, generated %d\n", name, ok_ ? "PASS" : "FAIL", st,      \
               pool_gen_count(b));                                                                   \
    } while (0)

    pool_write_key("pooltst", a, sizeof(*a));
    pool_read_key("pooltst", b, st, sizeof(st));
    POOL_CHECK("intact copy", !strcmp(st, "ok") && pool_gen_count(b) == gen0 && !memcmp(b->bshows, a->bshows, sizeof(a->bshows)));

    ((uint8_t *)a)[offsetof(PoolStore, gen) + 9] ^= 0x5a;
    pool_write_key("pooltst", a, sizeof(*a));
    pool_read_key("pooltst", b, st, sizeof(st));
    POOL_CHECK("flipped byte", !strncmp(st, "corrupt crc", 11) && pool_gen_count(b) == 0);
    ((uint8_t *)a)[offsetof(PoolStore, gen) + 9] ^= 0x5a;

    pool_write_key("pooltst", a, sizeof(*a) - 100);
    pool_read_key("pooltst", b, st, sizeof(st));
    POOL_CHECK("truncated blob", !strncmp(st, "corrupt size", 12) && pool_gen_count(b) == 0);

    uint8_t junk[200];
    esp_fill_random(junk, sizeof(junk));
    pool_write_key("pooltst", junk, sizeof(junk));
    pool_read_key("pooltst", b, st, sizeof(st));
    POOL_CHECK("random bytes", !strncmp(st, "corrupt size", 12) && pool_gen_count(b) == 0);

    memset(a, 0, sizeof(*a));
    a->version = POOL_VERSION;
    a->builtin_count = PET_QUOTE_COUNT;
    a->seed_hash = seed_hash();
    a->bshows[0] = 77; // impossible count -> reset to 0
    snprintf(a->gen[0].text, POOL_TEXT, "%s", "本鱼在这里，陪着你呢。");
    snprintf(a->gen[1].text, POOL_TEXT, "%s", "本鱼在这里，陪着你哦。");
    a->gen[1].tag = 0xff; // unknown bucket
    memset(a->gen[2].text, 'A', POOL_TEXT); // no terminator
    snprintf(a->gen[3].text, POOL_TEXT, "%s", "一二三四五六七八九十一二三四五六七八九十");
    a->crc = pool_crc(a);
    pool_write_key("pooltst", a, sizeof(*a));
    pool_read_key("pooltst", b, st, sizeof(st));
    POOL_CHECK("bad lines, good crc", strstr(st, "dropped 3") && pool_gen_count(b) == 1 && b->bshows[0] == 0);

    pool_read_key("pooltst_none", b, st, sizeof(st));
    POOL_CHECK("missing blob", !strcmp(st, "new") && pool_gen_count(b) == 0);
#undef POOL_CHECK
    nvs_handle_t handle;
    if (nvs_open(NVS_NS, NVS_READWRITE, &handle) == ESP_OK) {
        nvs_erase_key(handle, "pooltst");
        nvs_commit(handle);
        nvs_close(handle);
    }
    free(a);
    free(b);
    printf("POOL SELFTEST %s (%d/%d); live pool untouched\n", pass == total ? "PASS" : "FAIL", pass, total);
}

// Counters change on every sweep; write them 10 s after the last change (a few
// dozen 3 KB writes a day is far inside the NVS wear budget), so a power cut
// loses at most the last few seconds of show counts.
static int64_t g_pool_dirty_us;
static void pool_flush_if_due(void)
{
    if (g_pool_dirty && esp_timer_get_time() - g_pool_dirty_us > 10 * 1000000LL)
        pool_save();
}

static bool pool_active(int id)
{
    if (id < PET_QUOTE_COUNT)
        return g_pool.bshows[id] != POOL_RETIRED;
    return g_pool.gen[id - PET_QUOTE_COUNT].text[0] != 0;
}

static const char *pool_text(int id)
{
    return id < PET_QUOTE_COUNT ? PET_QUOTES[id].text : g_pool.gen[id - PET_QUOTE_COUNT].text;
}

static int pool_time(int id)
{
    return id < PET_QUOTE_COUNT ? PET_QUOTES[id].time : g_pool.gen[id - PET_QUOTE_COUNT].tag >> 4;
}

static int pool_cond(int id)
{
    return id < PET_QUOTE_COUNT ? PET_QUOTES[id].cond : g_pool.gen[id - PET_QUOTE_COUNT].tag & 15;
}

static uint8_t *pool_used(int id)
{
    return id < PET_QUOTE_COUNT ? &g_pool.bused[id] : &g_pool.gen[id - PET_QUOTE_COUNT].used;
}

static void pool_counts_locked(int *active, int *unused, int *generated)
{
    *active = *unused = *generated = 0;
    for (int id = 0; id < PET_QUOTE_COUNT + POOL_GEN_MAX; ++id) {
        if (!pool_active(id))
            continue;
        ++*active;
        if (!*pool_used(id))
            ++*unused;
        if (id >= PET_QUOTE_COUNT)
            ++*generated;
    }
}

static int time_bucket(int hour)
{
    if (hour >= 5 && hour <= 10)
        return T_MORNING;
    if (hour >= 11 && hour <= 13)
        return T_NOON;
    if (hour >= 14 && hour <= 16)
        return T_AFTERNOON;
    if (hour >= 17 && hour <= 19)
        return T_EVENING;
    if (hour >= 20 && hour <= 22)
        return T_NIGHT;
    return T_LATE;
}

// The moment a line is displayed: time bucket (T_ANY if the clock is not set
// yet), a bit mask of the conditions that hold now (1 << C_*), and the
// {temp}/{weather} values (empty while weather is unknown).
typedef struct {
    int bucket;
    unsigned conds;
    bool weather;
    char temp[12];
    char wname[16];
} PoolCtx;

static void context_now_locked(PoolCtx *ctx)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->bucket = T_ANY;
    if (time_ready()) {
        time_t wall = time(NULL);
        struct tm local;
        localtime_r(&wall, &local);
        ctx->bucket = time_bucket(local.tm_hour);
        if (local.tm_wday == 0 || local.tm_wday == 6)
            ctx->conds |= 1u << C_WEEKEND;
    }
    if (g_wcode >= 0) {
        bool snow = (g_wcode >= 71 && g_wcode <= 77) || g_wcode == 85 || g_wcode == 86;
        if (g_wcode >= 51 && !snow)
            ctx->conds |= 1u << C_RAIN;
        if (g_wcode <= 1)
            ctx->conds |= 1u << C_CLEAR;
        if (g_temp_c >= 30)
            ctx->conds |= 1u << C_HOT;
        if (g_temp_c <= 10)
            ctx->conds |= 1u << C_COLD;
        ctx->weather = true;
        snprintf(ctx->temp, sizeof(ctx->temp), "%d°C", g_temp_c);
        snprintf(ctx->wname, sizeof(ctx->wname), "%s", weather_name(g_wcode));
    }
}

static void context_text(const PoolCtx *ctx, char *out, size_t size)
{
    int used = snprintf(out, size, "%s", ctx->bucket == T_ANY ? "time-unknown" : T_NAMES[ctx->bucket]);
    for (int c = 1; c < C_COUNT && used < (int)size; ++c)
        if (ctx->conds & (1u << c))
            used += snprintf(out + used, size - used, " %s", C_NAMES[c]);
    if (!ctx->conds && used < (int)size)
        snprintf(out + used, size - used, " general");
}

// 0 = not eligible now; otherwise the pick weight: 1 for an any/general line,
// +2 for a line made for this time bucket, +2 for one made for this weather.
static int pool_weight(int id, const PoolCtx *ctx, char *filled, size_t size)
{
    if (!pool_active(id))
        return 0;
    int t = pool_time(id), c = pool_cond(id);
    if (t != T_ANY && t != ctx->bucket)
        return 0;
    if (c != C_GENERAL && !(ctx->conds & (1u << c)))
        return 0;
    const char *text = pool_text(id);
    if (has_placeholder(text)) {
        if (!ctx->weather || !fill_line(filled, size, text, ctx->temp, ctx->wname) || !display_bubble_fits(filled))
            return 0;
    } else {
        snprintf(filled, size, "%s", text);
    }
    return 1 + (t != T_ANY ? 2 : 0) + (c != C_GENERAL ? 2 : 0);
}

static bool pool_recent(int id)
{
    for (int i = 0; i < POOL_RECENT; ++i)
        if (g_recent[i] == id)
            return true;
    return false;
}

static int pool_choose_locked(const PoolCtx *ctx, bool need_unused, bool allow_recent)
{
    char filled[POOL_TEXT + 16];
    int chosen = -1;
    uint32_t total = 0;
    for (int id = 0; id < PET_QUOTE_COUNT + POOL_GEN_MAX; ++id) {
        if ((!allow_recent && pool_recent(id)) || (need_unused && *pool_used(id)))
            continue;
        int w = pool_weight(id, ctx, filled, sizeof(filled));
        if (!w)
            continue;
        total += (uint32_t)w;
        if (esp_random() % total < (uint32_t)w)
            chosen = id;
    }
    return chosen;
}

static void show_phrase_locked(void)
{
    PoolCtx ctx;
    context_now_locked(&ctx);
    int id = pool_choose_locked(&ctx, true, false);
    if (id < 0) {
        // Every eligible line was shown this round: start a new round for them.
        char filled[POOL_TEXT + 16];
        for (int k = 0; k < PET_QUOTE_COUNT + POOL_GEN_MAX; ++k)
            if (pool_weight(k, &ctx, filled, sizeof(filled)))
                *pool_used(k) = 0;
        id = pool_choose_locked(&ctx, true, false);
    }
    if (id < 0)
        id = pool_choose_locked(&ctx, false, false);
    if (id < 0)
        id = pool_choose_locked(&ctx, false, true);
    if (id < 0) {
        // Nothing fits right now (everything retired): bring the seed lines back.
        memset(g_pool.bshows, 0, sizeof(g_pool.bshows));
        memset(g_pool.bused, 0, sizeof(g_pool.bused));
        id = pool_choose_locked(&ctx, false, true);
    }
    char filled[POOL_TEXT + 16];
    if (id < 0 || !pool_weight(id, &ctx, filled, sizeof(filled))) {
        for (id = 0; id < PET_QUOTE_COUNT - 1; ++id)
            if (PET_QUOTES[id].time == T_ANY && PET_QUOTES[id].cond == C_GENERAL && !has_placeholder(PET_QUOTES[id].text))
                break;
        snprintf(filled, sizeof(filled), "%s", PET_QUOTES[id].text);
    }
    snprintf(g_phrase, sizeof(g_phrase), "%s", filled);
    g_phrase_until_us = esp_timer_get_time() + 7 * 1000000LL;
    *pool_used(id) = 1;
    g_recent[g_recent_pos] = id;
    g_recent_pos = (g_recent_pos + 1) % POOL_RECENT;
    uint8_t *shows = id < PET_QUOTE_COUNT ? &g_pool.bshows[id] : &g_pool.gen[id - PET_QUOTE_COUNT].shows;
    if (*shows < POOL_RETIRE)
        ++*shows;
    char now[64];
    context_text(&ctx, now, sizeof(now));
    printf("QUOTE #%d %s shows %d [%s/%s] now %s\n", id, g_phrase, *shows, T_NAMES[pool_time(id)],
           C_NAMES[pool_cond(id)], now);
    if (*shows >= POOL_RETIRE) {
        if (id < PET_QUOTE_COUNT) {
            g_pool.bshows[id] = POOL_RETIRED;
        } else {
            g_pool.gen[id - PET_QUOTE_COUNT].text[0] = 0;
        }
        printf("QUOTE #%d retired after %d shows\n", id, POOL_RETIRE);
    }
    int active, unused, generated;
    pool_counts_locked(&active, &unused, &generated);
    if (active < 8) {
        memset(g_pool.bshows, 0, sizeof(g_pool.bshows));
        pool_counts_locked(&active, &unused, &generated);
    }
    g_pool_dirty = true;
    g_pool_dirty_us = esp_timer_get_time();
    if (unused * 100 < active * POOL_LOW_PCT && g_key[0] && g_available && !g_refill_pending) {
        g_refill_pending = true;
        printf("POOL low: %d/%d unshown, batch refill queued\n", unused, active);
    }
}

static void dismiss_locked(void)
{
    int64_t now = esp_timer_get_time();
    if (g_remind_kind == 1) {
        ++g_drinks_today;
        g_last_drink_us = now;
        snprintf(g_phrase, sizeof(g_phrase), "喝了就好");
    } else if (g_remind_kind == 2) {
        g_last_move_us = now;
        snprintf(g_phrase, sizeof(g_phrase), "去走走吧");
    } else {
        snprintf(g_phrase, sizeof(g_phrase), "好啦知道");
    }
    g_remind[0] = 0;
    g_remind_kind = 0;
    g_phrase_until_us = now + 6 * 1000000LL;
}

static bool in_quiet(int hour)
{
    if (g_quiet_start == g_quiet_end)
        return false;
    if (g_quiet_start < g_quiet_end)
        return hour >= g_quiet_start && hour < g_quiet_end;
    return hour >= g_quiet_start || hour < g_quiet_end;
}

static void check_reminders(void)
{
    if (!time_ready())
        return;
    time_t wall = time(NULL);
    struct tm local;
    localtime_r(&wall, &local);
    int64_t now = esp_timer_get_time();
    xSemaphoreTake(mu, portMAX_DELAY);
    if (local.tm_yday != g_drink_day) {
        g_drink_day = local.tm_yday;
        g_drinks_today = 0;
    }
    if (g_remind[0] && now > g_remind_until_us) {
        if (g_remind_kind == 1)
            g_last_drink_us = now;
        if (g_remind_kind == 2)
            g_last_move_us = now;
        g_remind[0] = 0;
        g_remind_kind = 0;
    }
    if (g_rpause && g_remind[0]) {
        g_remind[0] = 0;
        g_remind_kind = 0;
    }
    if (g_rpause) {
        // Paused: keep the timers fresh so nothing fires the moment it resumes.
        g_last_drink_us = now;
        g_last_move_us = now;
    }
    if (!g_rpause && !g_remind[0] && !in_quiet(local.tm_hour)) {
        if (g_drink_min > 0 && g_last_drink_us &&
            now - g_last_drink_us >= (int64_t)g_drink_min * 60 * 1000000LL) {
            snprintf(g_remind, sizeof(g_remind), "该喝水了");
            g_remind_kind = 1;
            g_remind_until_us = now + 10 * 60 * 1000000LL;
        } else if (g_move_min > 0 && g_last_move_us &&
                   now - g_last_move_us >= (int64_t)g_move_min * 60 * 1000000LL) {
            snprintf(g_remind, sizeof(g_remind), "起来走走");
            g_remind_kind = 2;
            g_remind_until_us = now + 10 * 60 * 1000000LL;
        }
    }
    xSemaphoreGive(mu);
}

static void fetch_balance(const char *key)
{
    if (!key[0]) {
        xSemaphoreTake(mu, portMAX_DELAY);
        detail("密钥未设置");
        g_available = false;
        xSemaphoreGive(mu);
        return;
    }
    xSemaphoreTake(mu, portMAX_DELAY);
    detail("查询中");
    xSemaphoreGive(mu);

    int status = https_call(BALANCE_URL, key, 0);
    xSemaphoreTake(mu, portMAX_DELAY);
    if (status < 0) {
        detail("查询失败");
    } else if (status == 401) {
        detail("密钥无效");
        g_available = false;
    } else if (status == 402) {
        detail("不可用");
        g_available = false;
    } else if (status != 200) {
        snprintf(g_detail, sizeof(g_detail), "API %d", status);
        ESP_LOGW(TAG, "balance http %d", status);
    } else {
        cJSON *root = cJSON_ParseWithLength(body, (size_t)body_len);
        cJSON *infos = root ? cJSON_GetObjectItem(root, "balance_infos") : 0;
        cJSON *chosen = 0;
        cJSON *item;
        cJSON_ArrayForEach(item, infos) {
            cJSON *currency = cJSON_GetObjectItem(item, "currency");
            if (!chosen)
                chosen = item;
            if (cJSON_IsString(currency) && strcmp(currency->valuestring, "CNY") == 0) {
                chosen = item;
                break;
            }
        }
        if (!chosen) {
            detail("解析失败");
        } else {
            cJSON *total = cJSON_GetObjectItem(chosen, "total_balance");
            cJSON *granted = cJSON_GetObjectItem(chosen, "granted_balance");
            cJSON *topped = cJSON_GetObjectItem(chosen, "topped_up_balance");
            cJSON *currency = cJSON_GetObjectItem(chosen, "currency");
            cJSON *available = cJSON_GetObjectItem(root, "is_available");
            if (cJSON_IsString(total))
                snprintf(g_total, sizeof(g_total), "%s", total->valuestring);
            if (cJSON_IsString(granted))
                snprintf(g_granted, sizeof(g_granted), "%s", granted->valuestring);
            if (cJSON_IsString(topped))
                snprintf(g_topped, sizeof(g_topped), "%s", topped->valuestring);
            if (cJSON_IsString(currency))
                snprintf(g_currency, sizeof(g_currency), "%s", currency->valuestring);
            g_available = cJSON_IsTrue(available);
            if (time_ready()) {
                time_t wall = time(NULL);
                struct tm lt;
                localtime_r(&wall, &lt);
                snprintf(g_bal_hhmm, sizeof(g_bal_hhmm), "%02d:%02d", lt.tm_hour, lt.tm_min);
            }
            detail(g_available ? "" : "不可用");
            if (g_balance_bubble) {
                snprintf(g_phrase, sizeof(g_phrase), "%s", PHRASE_BALANCE);
                g_phrase_until_us = esp_timer_get_time() + 7 * 1000000LL;
            }
            ESP_LOGI(TAG, "balance %s %s available=%d", g_currency, g_total, g_available);
        }
        cJSON_Delete(root);
    }
    if (g_balance_bubble && !strcmp(g_phrase, PHRASE_BALANCE_WAIT)) {
        // Failed: show why instead of a stale "querying".
        snprintf(g_phrase, sizeof(g_phrase), "%s", g_detail[0] ? g_detail : "查询失败");
        g_phrase_until_us = esp_timer_get_time() + 6 * 1000000LL;
    }
    g_balance_bubble = false;
    xSemaphoreGive(mu);
}

static bool url_encode(char *out, size_t out_len, const char *in)
{
    static const char *hex = "0123456789ABCDEF";
    size_t used = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p; ++p) {
        int plain = (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9');
        size_t need = plain ? 1 : 3;
        if (used + need >= out_len)
            return false;
        if (plain) {
            out[used++] = (char)*p;
        } else {
            out[used++] = '%';
            out[used++] = hex[*p >> 4];
            out[used++] = hex[*p & 15];
        }
    }
    out[used] = 0;
    return true;
}

static void fmt_e6(char *out, size_t out_len, int e6)
{
    int neg = e6 < 0;
    int value = neg ? -e6 : e6;
    snprintf(out, out_len, "%s%d.%06d", neg ? "-" : "", value / 1000000, value % 1000000);
}

static const char *weather_name(int code)
{
    if (code <= 1)
        return "晴";
    if (code == 2)
        return "多云";
    if (code == 3)
        return "阴";
    if (code == 45 || code == 48)
        return "雾";
    if (code >= 51 && code <= 57)
        return "小雨";
    if (code >= 61 && code <= 67)
        return "雨";
    if (code >= 71 && code <= 77)
        return "雪";
    if (code >= 80 && code <= 82)
        return "阵雨";
    if (code == 85 || code == 86)
        return "阵雪";
    if (code >= 95 && code <= 99)
        return "雷雨";
    return "未知";
}

static int weather_icon(int code)
{
    if (code < 0)
        return ICON_NONE;
    if (code == 0)
        return ICON_SUN;
    if (code <= 2)
        return ICON_PARTLY;
    if (code == 3)
        return ICON_CLOUD;
    if (code == 45 || code == 48)
        return ICON_FOG;
    if ((code >= 71 && code <= 77) || code == 85 || code == 86)
        return ICON_SNOW;
    if (code >= 95)
        return ICON_THUNDER;
    return ICON_RAIN;
}

static void set_weather(const char *text)
{
    xSemaphoreTake(mu, portMAX_DELAY);
    snprintf(g_weather, sizeof(g_weather), "%s", text);
    xSemaphoreGive(mu);
}

static void fetch_weather(void)
{
    char city[32];
    int lat_e6;
    int lon_e6;
    int tz;
    bool has_geo;
    xSemaphoreTake(mu, portMAX_DELAY);
    memcpy(city, g_city, sizeof(city));
    lat_e6 = g_lat_e6;
    lon_e6 = g_lon_e6;
    tz = g_tz_hours;
    has_geo = g_has_geo || g_geo_fixed;
    xSemaphoreGive(mu);
    // Open-Meteo (free, no key, no DeepSeek): every WEATHER_EVERY after a good
    // fetch, WEATHER_RETRY after a failed one; immediately after Wi-Fi connects.
    int64_t delay = WEATHER_RETRY_US;
    if (!city[0])
        delay = WEATHER_EVERY_US;
    else if (!has_geo) {
        char encoded[160];
        char url[400];
        if (!url_encode(encoded, sizeof(encoded), city) ||
            snprintf(url, sizeof(url),
                     "https://geocoding-api.open-meteo.com/v1/search?name=%s&count=1&language=zh&format=json",
                     encoded) >= (int)sizeof(url)) {
            set_weather("天气失败");
        } else {
            int status = https_call(url, 0, 0);
            cJSON *root = status == 200 ? cJSON_ParseWithLength(body, (size_t)body_len) : 0;
            cJSON *results = root ? cJSON_GetObjectItem(root, "results") : 0;
            cJSON *first = cJSON_GetArrayItem(results, 0);
            cJSON *lat = first ? cJSON_GetObjectItem(first, "latitude") : 0;
            cJSON *lon = first ? cJSON_GetObjectItem(first, "longitude") : 0;
            if (!cJSON_IsNumber(lat) || !cJSON_IsNumber(lon)) {
                set_weather("天气失败");
            } else {
                lat_e6 = (int)(lat->valuedouble * 1000000.0);
                lon_e6 = (int)(lon->valuedouble * 1000000.0);
                has_geo = true;
                xSemaphoreTake(mu, portMAX_DELAY);
                g_lat_e6 = lat_e6;
                g_lon_e6 = lon_e6;
                g_has_geo = true;
                xSemaphoreGive(mu);
                char lat_text[16];
                char lon_text[16];
                snprintf(lat_text, sizeof(lat_text), "%d", lat_e6);
                snprintf(lon_text, sizeof(lon_text), "%d", lon_e6);
                nvs_put("lat", lat_text);
                nvs_put("lon", lon_text);
            }
            cJSON_Delete(root);
        }
    }
    if (city[0] && has_geo) {
        char lat_text[16];
        char lon_text[16];
        char url[320];
        fmt_e6(lat_text, sizeof(lat_text), lat_e6);
        fmt_e6(lon_text, sizeof(lon_text), lon_e6);
        snprintf(url, sizeof(url),
                 "https://api.open-meteo.com/v1/forecast?latitude=%s&longitude=%s"
                 "&current=temperature_2m,weather_code&timezone=%s",
                 lat_text, lon_text, tz == 9 ? "Asia%2FTokyo" : "Asia%2FShanghai");
        int status = https_call(url, 0, 0);
        cJSON *root = status == 200 ? cJSON_ParseWithLength(body, (size_t)body_len) : 0;
        cJSON *current = root ? cJSON_GetObjectItem(root, "current") : 0;
        cJSON *temp = current ? cJSON_GetObjectItem(current, "temperature_2m") : 0;
        cJSON *code = current ? cJSON_GetObjectItem(current, "weather_code") : 0;
        if (!cJSON_IsNumber(temp) || !cJSON_IsNumber(code)) {
            set_weather("天气失败");
        } else {
            double value = temp->valuedouble;
            int rounded = (int)(value + (value >= 0 ? 0.5 : -0.5));
            int wcode = (int)(code->valuedouble + (code->valuedouble >= 0 ? 0.5 : -0.5));
            char line[24];
            snprintf(line, sizeof(line), "%d°C %s", rounded, weather_name(wcode));
            set_weather(line);
            xSemaphoreTake(mu, portMAX_DELAY);
            g_wcode = wcode;
            g_temp_c = rounded;
            xSemaphoreGive(mu);
            xSemaphoreTake(mu, portMAX_DELAY);
            g_wx_today[0] = 0;
            g_wx_tomorrow[0] = 0;
            xSemaphoreGive(mu);
            delay = WEATHER_EVERY_US;
            g_weather_ok_us = esp_timer_get_time();
            ESP_LOGI(TAG, "weather %s (next in 30 min)", line);
        }
        cJSON_Delete(root);
    }
    xSemaphoreTake(mu, portMAX_DELAY);
    g_next_weather_us = esp_timer_get_time() + delay;
    xSemaphoreGive(mu);
    if (delay == WEATHER_RETRY_US)
        ESP_LOGW(TAG, "weather fetch failed, retry in 2 min");
}

static int llm_day_stamp(void)
{
    time_t wall = time(NULL);
    struct tm local;
    localtime_r(&wall, &local);
    return (local.tm_year + 1900) * 1000 + local.tm_yday;
}

// Daily cap, persisted so reboots cannot reset it. Returns false when spent.
static bool llm_take_slot(bool ignore_spacing)
{
    // 12 h spacing also survives reboots: last attempt time is kept in NVS.
    char last_text[16] = {0};
    nvs_load("llmt", last_text, sizeof(last_text));
    long long last = last_text[0] ? atoll(last_text) : 0;
    long long wall = (long long)time(NULL);
    if (!ignore_spacing && last && wall >= last && wall - last < 12LL * 3600)
        return false;
    int stamp = llm_day_stamp();
    bool ok;
    int count;
    xSemaphoreTake(mu, portMAX_DELAY);
    if (stamp != g_llm_day) {
        g_llm_day = stamp;
        g_llm_count = 0;
    }
    ok = g_llm_count < LLM_DAILY_CAP;
    if (ok)
        ++g_llm_count;
    count = g_llm_count;
    xSemaphoreGive(mu);
    if (ok) {
        char wall_text[16];
        snprintf(wall_text, sizeof(wall_text), "%lld", wall);
        nvs_put("llmt", wall_text);
        save_int("llmd", stamp);
        save_int("llmn", count);
    }
    return ok;
}

// Cleans a generated line in place; false when it must be rejected. Lines may
// use {temp} / {weather}; they are measured filled with the widest values.
static bool pool_line_ok(char *text)
{
    static const char *const banned[] = {"政", "党", "主席", "国家", "币", "股", "投资", "理财", "色情", "性感",
                                         "赌", "毒", "枪", "广告", "http", "AI", "模型", "生日", "{n}"};
    char clean[POOL_TEXT];
    int used = 0, count = 0;
    const unsigned char *cursor = (const unsigned char *)text;
    uint32_t c;
    while (*cursor) {
        if (!utf8_next(&cursor, &c))
            return false;
        if (c >= 0x300 && c <= 0x36f)
            continue; // combining marks (e.g. in •̀) have no glyph
        if (c < 32 || c > 0xffff || c == '"' || c == '\\')
            return false;
        if (c == ' ' && (used == 0 || clean[used - 1] == ' '))
            continue;
        utf8_put(clean, &used, sizeof(clean), c);
        ++count;
        if (used >= POOL_TEXT - 4)
            return false;
    }
    while (used && clean[used - 1] == ' ')
        --used;
    clean[used] = 0;
    if (count < 4)
        return false;
    for (unsigned i = 0; i < sizeof(banned) / sizeof(banned[0]); ++i)
        if (strstr(clean, banned[i]))
            return false;
    char filled[POOL_TEXT + 16];
    if (!fill_line(filled, sizeof(filled), clean, FILL_TEMP_WORST, FILL_WEATHER_WORST))
        return false; // unknown {placeholder} or stray brace
    if (!display_bubble_fits(filled))
        return false; // needs more than two bubble lines, or unknown glyphs
    strcpy(text, clean);
    return true;
}

static int parse_time_tag(const char *t)
{
    if (!t)
        return T_ANY;
    if (!strcmp(t, "late-night") || !strcmp(t, "late_night") || !strcmp(t, "latenight") || !strcmp(t, "late"))
        return T_LATE;
    for (int i = 0; i < T_COUNT; ++i)
        if (!strcmp(t, T_NAMES[i]))
            return i;
    return T_ANY;
}

static int parse_cond_tag(const char *t)
{
    if (!t)
        return C_GENERAL;
    for (int i = 0; i < C_COUNT; ++i)
        if (!strcmp(t, C_NAMES[i]))
            return i;
    return C_GENERAL;
}

// Keeps a tag honest with its text: a line that says 早上 is a morning line, one
// that mentions rain is a rain line. Fills in an any/general tag from the text
// and rejects a line whose words contradict its tag.
static bool tag_consistent(const char *text, int *time_tag, int *cond_tag)
{
    static const struct {
        const char *word;
        unsigned times; // allowed time buckets (1 << T_*)
    } time_words[] = {
        {"早上", 1u << T_MORNING},     {"早安", 1u << T_MORNING},
        {"起床", 1u << T_MORNING},     {"早餐", 1u << T_MORNING},
        {"中午", 1u << T_NOON},        {"午饭", 1u << T_NOON},
        {"午餐", 1u << T_NOON},        {"下午", 1u << T_AFTERNOON},
        {"傍晚", 1u << T_EVENING},     {"晚饭", 1u << T_EVENING},
        {"今晚", 1u << T_EVENING | 1u << T_NIGHT},
        {"晚上", 1u << T_EVENING | 1u << T_NIGHT | 1u << T_LATE},
        {"晚安", 1u << T_NIGHT | 1u << T_LATE},
        {"深夜", 1u << T_LATE},        {"熬夜", 1u << T_NIGHT | 1u << T_LATE},
        {"凌晨", 1u << T_LATE},        {"半夜", 1u << T_LATE},
    };
    unsigned allowed = 0;
    for (unsigned i = 0; i < sizeof(time_words) / sizeof(time_words[0]); ++i)
        if (strstr(text, time_words[i].word))
            allowed = allowed ? allowed & time_words[i].times : time_words[i].times;
    if (allowed && !(allowed & (1u << *time_tag))) {
        if (*time_tag != T_ANY)
            return false;
        for (int t = 1; t < T_COUNT; ++t)
            if (allowed & (1u << t)) {
                *time_tag = t;
                break;
            }
    }
    int implied = -1;
    if (strstr(text, "雨") || strstr(text, "带伞"))
        implied = C_RAIN;
    else if (strstr(text, "周末"))
        implied = C_WEEKEND;
    if (implied >= 0 && *cond_tag != implied) {
        if (*cond_tag != C_GENERAL)
            return false;
        *cond_tag = implied;
    }
    return true;
}

static bool pool_has_locked(const char *text)
{
    for (int id = 0; id < PET_QUOTE_COUNT + POOL_GEN_MAX; ++id)
        if ((id < PET_QUOTE_COUNT || pool_active(id)) && !strcmp(pool_text(id), text))
            return true;
    return false;
}

// Free slot for a new generated line: empty first, else the most-shown one
// that has already been seen this round.
static int pool_free_slot_locked(void)
{
    int best = -1, best_shows = 0;
    for (int i = 0; i < POOL_GEN_MAX; ++i) {
        GenLine *g = &g_pool.gen[i];
        if (!g->text[0])
            return i;
        if (g->used && g->shows >= 2 && g->shows > best_shows) {
            best = i;
            best_shows = g->shows;
        }
    }
    return best;
}

// Appends src to out, replacing every {{name}} with the matching value.
static void prompt_fill(char *out, size_t size, const char *tpl, const char *const *names,
                        const char *const *values, int count)
{
    size_t used = 0;
    while (*tpl && used + 1 < size) {
        if (tpl[0] == '{' && tpl[1] == '{') {
            const char *end = strstr(tpl + 2, "}}");
            if (end) {
                size_t n = (size_t)(end - tpl - 2);
                for (int i = 0; i < count; ++i)
                    if (strlen(names[i]) == n && !strncmp(tpl + 2, names[i], n)) {
                        size_t v = strlen(values[i]);
                        if (used + v >= size)
                            v = size - used - 1;
                        memcpy(out + used, values[i], v);
                        used += v;
                        break;
                    }
                tpl = end + 2;
                continue;
            }
        }
        out[used++] = *tpl++;
    }
    out[used] = 0;
}

static const char *season_name(int month)
{
    static const char *const names[12] = {"隆冬", "冬末春初", "早春", "春天", "春末夏初", "初夏",
                                          "盛夏", "盛夏", "初秋", "秋天", "深秋初冬", "冬天"};
    return month >= 1 && month <= 12 ? names[month - 1] : "未知";
}

static bool birthday_now_locked(bool have_time, const struct tm *local);

// The user prompt for one refill: PET_USER_PROMPT with the current context.
static char *pool_user_prompt(void)
{
    char *avoid = malloc(800);
    char *ask = malloc(4096);
    if (!avoid || !ask) {
        free(avoid);
        free(ask);
        return 0;
    }
    char date[32] = "未知", weekday[16] = "", clock[8] = "", daytype[16] = "", season[16] = "未知";
    char place[40], weather_now[48], count[8], mode[64];
    const char *bucket = "any", *wclass = "general";
    int alen = 0;
    avoid[0] = 0;
    xSemaphoreTake(mu, portMAX_DELAY);
    bool have_time = time_ready();
    struct tm local = {0};
    if (have_time) {
        static const char *const wd[7] = {"星期日", "星期一", "星期二", "星期三", "星期四", "星期五", "星期六"};
        time_t wall = time(NULL);
        localtime_r(&wall, &local);
        snprintf(date, sizeof(date), "%d年%d月%d日", local.tm_year + 1900, local.tm_mon + 1, local.tm_mday);
        snprintf(weekday, sizeof(weekday), "%s", wd[local.tm_wday % 7]);
        snprintf(clock, sizeof(clock), "%02d:%02d", local.tm_hour, local.tm_min);
        snprintf(daytype, sizeof(daytype), "%s", local.tm_wday == 0 || local.tm_wday == 6 ? "周末" : "工作日");
        snprintf(season, sizeof(season), "%s", season_name(local.tm_mon + 1));
        bucket = T_NAMES[time_bucket(local.tm_hour)];
    }
    PoolCtx ctx;
    context_now_locked(&ctx);
    static const int order[] = {C_RAIN, C_HOT, C_COLD, C_CLEAR};
    for (unsigned i = 0; i < sizeof(order) / sizeof(order[0]); ++i)
        if (ctx.conds & (1u << order[i])) {
            wclass = C_NAMES[order[i]];
            break;
        }
    snprintf(place, sizeof(place), "%s", g_city[0] ? g_city : "未知");
    if (ctx.weather)
        snprintf(weather_now, sizeof(weather_now), "%s %s", ctx.temp, ctx.wname);
    else
        snprintf(weather_now, sizeof(weather_now), "未知");
    snprintf(mode, sizeof(mode), "%s", birthday_now_locked(have_time, &local) ? "生日模式（这批句子留到平时用）" : "陪伴模式");
    for (int i = 0; i < POOL_RECENT; ++i)
        if (g_recent[i] >= 0 && pool_active(g_recent[i]) && alen < 700)
            alen += snprintf(avoid + alen, 800 - alen, "%s；", pool_text(g_recent[i]));
    for (int i = 0; i < POOL_GEN_MAX && alen < 700; ++i)
        if (g_pool.gen[i].text[0] && (esp_random() % 3) == 0)
            alen += snprintf(avoid + alen, 800 - alen, "%s；", g_pool.gen[i].text);
    for (int i = 0; i < PET_QUOTE_COUNT && alen < 700; ++i)
        if ((esp_random() % 4) == 0)
            alen += snprintf(avoid + alen, 800 - alen, "%s；", PET_QUOTES[i].text);
    xSemaphoreGive(mu);
    snprintf(count, sizeof(count), "%d", POOL_BATCH);
    static const char *const names[] = {"date", "weekday", "clock", "daytype", "bucket", "season", "place",
                                        "weather_now", "wclass", "mode", "count", "avoid"};
    const char *values[] = {date, weekday, clock, daytype, bucket, season, place,
                            weather_now, wclass, mode, count, avoid[0] ? avoid : "无"};
    prompt_fill(ask, 4096, PET_USER_PROMPT, names, values, (int)(sizeof(names) / sizeof(names[0])));
    free(avoid);
    return ask;
}

static void pool_refill(const char *key)
{
    bool force;
    xSemaphoreTake(mu, portMAX_DELAY);
    force = g_refill_force;
    g_refill_force = false;
    xSemaphoreGive(mu);
    if (!llm_take_slot(force)) {
        ESP_LOGI(TAG, "pool refill skipped: 12 h spacing or daily cap %d", LLM_DAILY_CAP);
        xSemaphoreTake(mu, portMAX_DELAY);
        g_refill_pending = false;
        xSemaphoreGive(mu);
        return;
    }
    char *ask = pool_user_prompt();
    cJSON *root = ask ? cJSON_CreateObject() : 0;
    if (!root) {
        free(ask);
        xSemaphoreTake(mu, portMAX_DELAY);
        g_refill_pending = false;
        xSemaphoreGive(mu);
        return;
    }
    const char *cut = strstr(ask, "\n\n");
    printf("POOL refill prompt context:\n%.*s\n", cut ? (int)(cut - ask) : 0, ask);
    cJSON_AddStringToObject(root, "model", LLM_MODEL);
    cJSON *thinking = cJSON_AddObjectToObject(root, "thinking");
    cJSON_AddStringToObject(thinking, "type", "disabled");
    cJSON_AddNumberToObject(root, "temperature", 1.3);
    cJSON_AddNumberToObject(root, "max_tokens", 1500);
    cJSON *format = cJSON_AddObjectToObject(root, "response_format");
    cJSON_AddStringToObject(format, "type", "json_object");
    cJSON *messages = cJSON_AddArrayToObject(root, "messages");
    cJSON *sys = cJSON_CreateObject();
    cJSON_AddStringToObject(sys, "role", "system");
    cJSON_AddStringToObject(sys, "content", PET_SYSTEM_PROMPT);
    cJSON_AddItemToArray(messages, sys);
    cJSON *user = cJSON_CreateObject();
    cJSON_AddStringToObject(user, "role", "user");
    cJSON_AddStringToObject(user, "content", ask);
    cJSON_AddItemToArray(messages, user);
    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    free(ask);
    int status = payload ? https_call("https://api.deepseek.com/chat/completions", key, payload) : -1;
    free(payload);
    if (status != 200)
        printf("POOL refill http %d: %.200s\n", status, body);
    int added = 0, rejected = 0;
    if (status == 200) {
        cJSON *resp = cJSON_ParseWithLength(body, (size_t)body_len);
        cJSON *choices = resp ? cJSON_GetObjectItem(resp, "choices") : 0;
        cJSON *first = cJSON_GetArrayItem(choices, 0);
        cJSON *message = first ? cJSON_GetObjectItem(first, "message") : 0;
        cJSON *content = message ? cJSON_GetObjectItem(message, "content") : 0;
        cJSON *batch = cJSON_IsString(content) ? cJSON_Parse(content->valuestring) : 0;
        cJSON_Delete(resp);
        cJSON *list = batch ? cJSON_GetObjectItem(batch, "l") : 0;
        cJSON *item;
        cJSON_ArrayForEach(item, list)
        {
            const char *ttext = 0, *ctext = 0, *text = 0;
            if (cJSON_IsArray(item) && cJSON_GetArraySize(item) >= 3) {
                cJSON *t = cJSON_GetArrayItem(item, 0), *w = cJSON_GetArrayItem(item, 1),
                      *s = cJSON_GetArrayItem(item, 2);
                ttext = cJSON_IsString(t) ? t->valuestring : 0;
                ctext = cJSON_IsString(w) ? w->valuestring : 0;
                text = cJSON_IsString(s) ? s->valuestring : 0;
            } else if (cJSON_IsObject(item)) {
                cJSON *t = cJSON_GetObjectItem(item, "t"), *w = cJSON_GetObjectItem(item, "w"),
                      *s = cJSON_GetObjectItem(item, "s");
                ttext = cJSON_IsString(t) ? t->valuestring : 0;
                ctext = cJSON_IsString(w) ? w->valuestring : 0;
                text = cJSON_IsString(s) ? s->valuestring : 0;
            } else if (cJSON_IsString(item)) {
                text = item->valuestring;
            }
            char line[POOL_TEXT * 2];
            if (!text || strlen(text) >= sizeof(line)) {
                ++rejected;
                continue;
            }
            strcpy(line, text);
            int ttag = parse_time_tag(ttext), ctag = parse_cond_tag(ctext);
            if (!pool_line_ok(line) || !tag_consistent(line, &ttag, &ctag)) {
                printf("POOL reject [%s/%s] %s\n", ttext ? ttext : "?", ctext ? ctext : "?", text);
                ++rejected;
                continue;
            }
            xSemaphoreTake(mu, portMAX_DELAY);
            int slot = pool_has_locked(line) ? -2 : pool_free_slot_locked();
            if (slot >= 0) {
                GenLine *g = &g_pool.gen[slot];
                memset(g, 0, sizeof(*g));
                g->tag = (uint8_t)(ttag << 4 | ctag);
                snprintf(g->text, sizeof(g->text), "%s", line);
                ++added;
            }
            xSemaphoreGive(mu);
            if (slot == -2)
                ++rejected;
            if (slot >= 0)
                printf("POOL add [%s/%s] %s\n", T_NAMES[ttag], C_NAMES[ctag], line);
        }
        cJSON_Delete(batch);
    } else if (status == 401 || status == 402) {
        xSemaphoreTake(mu, portMAX_DELAY);
        g_available = false;
        detail(status == 401 ? "密钥无效" : "不可用");
        xSemaphoreGive(mu);
    }
    xSemaphoreTake(mu, portMAX_DELAY);
    g_refill_pending = false;
    if (added && g_pool.refills < 65535)
        ++g_pool.refills;
    g_pool_dirty = true;
    xSemaphoreGive(mu);
    pool_save();
    ESP_LOGI(TAG, "pool refill http %d added %d rejected %d", status, added, rejected);
}

// On-demand WPA2 setup hotspot. Only while there is no working home Wi-Fi:
// a permanent AP+STA shares one radio and is forced onto the router's channel.
static void ap_start(void)
{
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    char ssid[24];
    char pass[12];
    snprintf(ssid, sizeof(ssid), "Pet-Setup-%02X%02X", mac[4], mac[5]);
    snprintf(pass, sizeof(pass), "%08lu", (unsigned long)(esp_random() % 100000000UL));
    wifi_config_t ap;
    memset(&ap, 0, sizeof(ap));
    memcpy(ap.ap.ssid, ssid, strlen(ssid));
    ap.ap.ssid_len = (uint8_t)strlen(ssid);
    memcpy(ap.ap.password, pass, strlen(pass));
    ap.ap.channel = 6;
    ap.ap.max_connection = 2;
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ap.ap.pmf_cfg.required = false;
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err == ESP_OK)
        err = esp_wifi_set_config(WIFI_IF_AP, &ap);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "setup hotspot failed: %s", esp_err_to_name(err));
        esp_wifi_set_mode(WIFI_MODE_STA);
        xSemaphoreTake(mu, portMAX_DELAY);
        g_ap_next_ok_us = esp_timer_get_time() + AP_REARM_US;
        xSemaphoreGive(mu);
        return;
    }
    xSemaphoreTake(mu, portMAX_DELAY);
    g_ap_active = true;
    g_ap_want_off = false;
    g_ap_clients = 0;
    g_ap_until_us = esp_timer_get_time() + AP_LIFETIME_US;
    snprintf(g_ap_ssid, sizeof(g_ap_ssid), "%s", ssid);
    snprintf(g_ap_pass, sizeof(g_ap_pass), "%s", pass);
    xSemaphoreGive(mu);
    // The password is temporary and also shown on the screen.
    printf("setup hotspot on: %s password %s http://192.168.4.1/ (auto-off in 10 min)\n", ssid, pass);
    fflush(stdout);
}

static void ap_stop(void)
{
    esp_wifi_set_mode(WIFI_MODE_STA);
    xSemaphoreTake(mu, portMAX_DELAY);
    g_ap_active = false;
    g_ap_want_off = false;
    g_ap_clients = 0;
    g_ap_pass[0] = 0;
    g_ap_next_ok_us = esp_timer_get_time() + AP_REARM_US;
    if (!g_connected && g_ssid[0]) {
        g_sta_down_since_us = esp_timer_get_time();
        g_retry_at_us = esp_timer_get_time() + 500000;
    }
    xSemaphoreGive(mu);
    printf("setup hotspot off\n");
    fflush(stdout);
}

static void ap_tick(void)
{
    int64_t now = esp_timer_get_time();
    bool start = false;
    bool stop = false;
    xSemaphoreTake(mu, portMAX_DELAY);
    if (g_ap_active) {
        if (g_ap_stop_request)
            stop = true;
        else if (now >= g_ap_until_us && (g_ap_want_off || g_ap_clients == 0))
            stop = true;
        else if (now >= g_ap_until_us)
            g_ap_until_us = now + 2LL * 60 * 1000000LL; // someone is still on it
    } else if (g_ap_request) {
        start = true;
    } else if (!g_connected && !g_scan_in_progress && now >= g_ap_next_ok_us && now > 5000000) {
        if (!g_ssid[0])
            start = true;
        else if (g_sta_down_since_us && now - g_sta_down_since_us >= AP_FAIL_US)
            start = true;
    }
    g_ap_request = false;
    g_ap_stop_request = false;
    xSemaphoreGive(mu);
    if (start)
        ap_start();
    else if (stop)
        ap_stop();
}

static void net_task(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
        check_reminders();
        pool_flush_if_due();
        ap_tick();
        bool connected;
        bool pending;
        bool retry;
        bool weather;
        bool quote;
        char key[128];
        char ssid[sizeof(g_ssid)];
        xSemaphoreTake(mu, portMAX_DELAY);
        connected = g_connected;
        pending = g_fetch_pending;
        quote = g_refill_pending;
        memcpy(key, g_key, sizeof(key));
        memcpy(ssid, g_ssid, sizeof(ssid));
        int64_t now = esp_timer_get_time();
        // While the setup hotspot runs, do not keep re-scanning for the home
        // network: every scan hops channels and drops the phone.
        retry = !connected && !g_scan_in_progress && !g_ap_active && ssid[0] && g_retry_at_us &&
                now >= g_retry_at_us;
        if (retry) {
            g_retry_at_us = now + 8000000;
            label("连接中");
        }
        if (connected && pending)
            g_fetch_pending = false;
        weather = connected && g_city[0] && now >= g_next_weather_us && !pending && !quote;
        if (weather)
            g_next_weather_us = now + 90 * 1000000LL;
        xSemaphoreGive(mu);
        if (retry)
            start_connect();
        if (!connected)
            continue;
        if (!time_ready()) {
            ensure_sntp();
            xSemaphoreTake(mu, portMAX_DELAY);
            if (pending) {
                g_fetch_pending = true;
                detail("等待校时");
            }
            if (weather)
                g_next_weather_us = 0;
            xSemaphoreGive(mu);
            continue;
        }
        if (pending)
            fetch_balance(key);
        else if (quote)
            pool_refill(key);
        else if (weather)
            fetch_weather();
    }
}

static void copy_field(char *dest, size_t dest_len, const char *src)
{
    snprintf(dest, dest_len, "%s", src);
}

static const char *auth_name(wifi_auth_mode_t mode)
{
    switch (mode) {
    case WIFI_AUTH_OPEN: return "OPEN";
    case WIFI_AUTH_WEP: return "WEP";
    case WIFI_AUTH_WPA_PSK: return "WPA";
    case WIFI_AUTH_WPA2_PSK: return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK: return "WPA/WPA2";
    case WIFI_AUTH_WPA3_PSK: return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK: return "WPA2/WPA3";
    case WIFI_AUTH_OWE: return "OWE";
    default: return "OTHER";
    }
}

static void scan_wifi_task(void *arg)
{
    (void)arg;
    char target[sizeof(g_ssid)];
    bool was_connected;
    xSemaphoreTake(mu, portMAX_DELAY);
    copy_field(target, sizeof(target), g_ssid);
    was_connected = g_connected;
    xSemaphoreGive(mu);

    if (!was_connected) {
        (void)esp_wifi_disconnect();
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    wifi_scan_config_t config = {
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active = {.min = 100, .max = 180},
    };
    esp_err_t err = esp_wifi_scan_start(&config, true);
    if (err != ESP_OK) {
        printf("scan failed: %s\n", esp_err_to_name(err));
    } else {
        uint16_t total = 0;
        err = esp_wifi_scan_get_ap_num(&total);
        if (err != ESP_OK) {
            printf("scan result failed: %s\n", esp_err_to_name(err));
        } else {
            uint16_t count = total < 24 ? total : 24;
            err = esp_wifi_scan_get_ap_records(&count, g_scan_records);
            if (err != ESP_OK) {
                printf("scan result failed: %s\n", esp_err_to_name(err));
            } else {
                printf("scan: %u networks in 2.4 GHz; * marks %s\n", (unsigned)total,
                       target[0] ? target : "no saved SSID");
                for (uint16_t i = 0; i < count; ++i) {
                    char name[sizeof(g_scan_records[i].ssid) + 1];
                    memcpy(name, g_scan_records[i].ssid, sizeof(g_scan_records[i].ssid));
                    name[sizeof(g_scan_records[i].ssid)] = 0;
                    printf("%c ch=%u rssi=%d auth=%s ssid=", strcmp(name, target) == 0 ? '*' : ' ',
                           g_scan_records[i].primary, g_scan_records[i].rssi,
                           auth_name(g_scan_records[i].authmode));
                    for (size_t j = 0; j < sizeof(g_scan_records[i].ssid) && g_scan_records[i].ssid[j]; ++j) {
                        unsigned char c = g_scan_records[i].ssid[j];
                        putchar((c < 0x20 || c == 0x7f) ? '?' : c);
                    }
                    putchar('\n');
                }
                if (total > count)
                    printf("showing strongest %u of %u networks\n", (unsigned)count, (unsigned)total);
            }
        }
    }
    fflush(stdout);

    xSemaphoreTake(mu, portMAX_DELAY);
    g_scan_in_progress = false;
    if (!g_connected && g_ssid[0])
        g_retry_at_us = esp_timer_get_time() + 1000000;
    xSemaphoreGive(mu);
    vTaskDelete(NULL);
}

static void start_scan_wifi(void)
{
    xSemaphoreTake(mu, portMAX_DELAY);
    if (g_scan_in_progress) {
        xSemaphoreGive(mu);
        printf("scan already running\n");
        fflush(stdout);
        return;
    }
    g_scan_in_progress = true;
    g_retry_at_us = 0;
    xSemaphoreGive(mu);

    if (xTaskCreate(scan_wifi_task, "wifi_scan", 8192, 0, 4, 0) != pdPASS) {
        xSemaphoreTake(mu, portMAX_DELAY);
        g_scan_in_progress = false;
        if (!g_connected && g_ssid[0])
            g_retry_at_us = esp_timer_get_time() + 1000000;
        xSemaphoreGive(mu);
        printf("could not start scan task\n");
        fflush(stdout);
        return;
    }
    printf("scan started; results will follow\n");
    fflush(stdout);
}

void net_init(void)
{

    mu = xSemaphoreCreateMutex();
    esp_err_t err = nvs_flash_init();
    if (err != ESP_OK) {
        // Full, newer-format or unreadable NVS: start clean instead of a boot loop.
        printf("NVS init failed (%s), erasing settings\n", esp_err_to_name(err));
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }
    {
        static const char *const reasons[] = {"unknown", "power-on", "external", "software", "panic",
                                              "int-watchdog", "task-watchdog", "watchdog", "deep-sleep",
                                              "brownout", "sdio", "usb", "jtag", "efuse", "power-glitch", "cpu-lockup"};
        esp_reset_reason_t why = esp_reset_reason();
        g_reset_name = (unsigned)why < sizeof(reasons) / sizeof(reasons[0]) ? reasons[why] : "other";
        g_boots = load_int("boots", 0, 0, 999999) + 1;
        save_int("boots", g_boots % 1000000);
    }
    nvs_load("ssid", g_ssid, sizeof(g_ssid));
    nvs_load("pass", g_pass, sizeof(g_pass));
    nvs_load("key", g_key, sizeof(g_key));
    nvs_load("city", g_city, sizeof(g_city));
    pool_load();
    g_drink_min = load_int("drink", 45, 0, 240);
    g_move_min = load_int("move", 60, 0, 240);
    g_quiet_start = load_int("q0", 23, 0, 23);
    g_quiet_end = load_int("q1", 8, 0, 23);
    g_tz_hours = load_int("tz", 8, 8, 9);
    if (g_tz_hours != 9)
        g_tz_hours = 8;
    apply_tz();
    char lat_text[16] = {0};
    char lon_text[16] = {0};
    nvs_load("lat", lat_text, sizeof(lat_text));
    nvs_load("lon", lon_text, sizeof(lon_text));
    if (lat_text[0] && lon_text[0]) {
        int lat = atoi(lat_text), lon = atoi(lon_text);
        if (lat >= -90000000 && lat <= 90000000 && lon >= -180000000 && lon <= 180000000 && (lat || lon)) {
            g_lat_e6 = lat;
            g_lon_e6 = lon;
            g_has_geo = true;
        } else {
            printf("stored coordinates invalid, ignored\n");
        }
    }
    g_geo_fixed = g_has_geo && load_int("geofix", 0, 0, 1) == 1;
    g_mode = load_int("mode", 0, 0, 1);
    nvs_load("bname", g_bname, sizeof(g_bname));
    g_bmonth = load_int("bmon", 0, 0, 12);
    g_bday = load_int("bday", 0, 0, 31);
    {
        static const int8_t mdays[13] = {0, 31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
        if (!g_bmonth || !g_bday || g_bday > mdays[g_bmonth])
            g_bmonth = g_bday = 0;
    }
    g_bauto = load_int("bauto", 1, 0, 1) == 1;
    nvs_load("msg", g_msg, sizeof(g_msg));
    g_rpause = load_int("rpause", 0, 0, 1) == 1;
    {
        char stamp[12] = {0};
        nvs_load("llmd", stamp, sizeof(stamp));
        if (stamp[0]) {
            g_llm_day = atoi(stamp);
            g_llm_count = load_int("llmn", 0, 0, 1000);
        }
    }
    if (g_ssid[0])
        g_sta_down_since_us = esp_timer_get_time();
    g_last_drink_us = esp_timer_get_time();
    g_last_move_us = g_last_drink_us;
    label(g_ssid[0] ? "连接中" : "未设置");
    detail("");

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();
    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, 0));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, 0));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    if (g_ssid[0] && (!g_pass[0] || strlen(g_pass) >= 8)) {
        wifi_config_t sta;
        fill_sta_config(&sta, g_ssid, g_pass);
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta));
    }
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_LOGI(TAG, "Wi-Fi power save disabled");
    // ESP-IDF takes quarter-dBm; on ESP32-C3, 34 (8.5 dBm request) maps to
    // the nearest supported RF tier below 8.5 dBm.
    ESP_ERROR_CHECK(esp_wifi_set_max_tx_power(34));
    int8_t tx_power = 0;
    ESP_ERROR_CHECK(esp_wifi_get_max_tx_power(&tx_power));
    ESP_LOGI(TAG, "Wi-Fi max TX power: %d quarter-dBm", tx_power);
    if (mdns_init() == ESP_OK) {
        mdns_hostname_set("pet");
        mdns_instance_name_set("desk pet");
        mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
        ESP_LOGI(TAG, "mDNS http://pet.local/");
    } else {
        ESP_LOGW(TAG, "mDNS init failed");
    }
    portal_start();
    xTaskCreate(net_task, "net", 20480, 0, 5, &net_task_handle);
    printf("PET READY v0.5\n");
    printf("boot #%d reset %s; pool %s\n", g_boots, g_reset_name, g_pool_boot);
    printf("Settings: http://pet.local/ or http://<device-ip>/ on the home network; USB serial also works.\n");
    printf("SSID PASS KEY CITY LOC DRINK MOVE QUIET TZ MODE BNAME BDAY BAUTO MSG PAUSE SWEEP SONAR PET POOL BDAYDEMO AP FETCH STATUS SCAN FORGET WEBTEST\n");
    fflush(stdout);
}

void net_request_fetch(void)
{
    xSemaphoreTake(mu, portMAX_DELAY);
    queue_fetch(15 * 1000000LL);
    xSemaphoreGive(mu);
}

// Days from today to the next birthday (0 = today). -1 when unset or no time.
static int days_to_birthday_locked(const struct tm *today)
{
    if (!g_bmonth || !g_bday)
        return -1;
    struct tm a = *today;
    a.tm_hour = 12;
    a.tm_min = a.tm_sec = 0;
    a.tm_isdst = 0;
    time_t now = mktime(&a);
    for (int add = 0; add < 2; ++add) {
        struct tm b = a;
        b.tm_year = today->tm_year + add;
        b.tm_mon = g_bmonth - 1;
        b.tm_mday = g_bday;
        time_t then = mktime(&b);
        if (then >= now)
            return (int)((then - now + 43200) / 86400);
    }
    return -1;
}

// One of the 10 birthday wishes, {n} -> configured name, no repeat until all
// have been shown (and never the same one twice in a row).
static void birthday_wish_locked(void)
{
    const uint16_t all = (uint16_t)((1u << BIRTHDAY_WISH_COUNT) - 1);
    if ((g_wish_used & all) == all)
        g_wish_used = 0;
    int pick = -1, seen = 0;
    for (int i = 0; i < BIRTHDAY_WISH_COUNT; ++i) {
        if ((g_wish_used & (1u << i)) || (i == g_wish_last && BIRTHDAY_WISH_COUNT > 1))
            continue;
        if (esp_random() % (uint32_t)++seen == 0)
            pick = i;
    }
    if (pick < 0)
        pick = (g_wish_last + 1) % BIRTHDAY_WISH_COUNT;
    g_wish_used |= (uint16_t)(1u << pick);
    g_wish_last = pick;
    const char *name = g_bname[0] ? g_bname : "你";
    char line[sizeof(g_phrase)];
    int used = 0;
    for (const char *p = BIRTHDAY_WISHES[pick]; *p && used < (int)sizeof(line) - 1;) {
        if (p[0] == '{' && p[1] == 'n' && p[2] == '}') {
            used += snprintf(line + used, sizeof(line) - used, "%s", name);
            if (used >= (int)sizeof(line))
                used = sizeof(line) - 1;
            p += 3;
        } else {
            line[used++] = *p++;
        }
    }
    line[used] = 0;
    // A long custom name may not fit two lines: fall back to the short form.
    if (!display_bubble_fits(line))
        snprintf(line, sizeof(line), "%s\n生日快乐\xf0\x9f\x8e\x82", name);
    if (!display_bubble_fits(line))
        snprintf(line, sizeof(line), "生日快乐\xf0\x9f\x8e\x82");
    snprintf(g_phrase, sizeof(g_phrase), "%s", line);
    printf("WISH #%d %s\n", pick, g_phrase);
}

static bool birthday_now_locked(bool have_time, const struct tm *local)
{
    if (g_mode == 1 || (g_demo_until_us && esp_timer_get_time() < g_demo_until_us))
        return true;
    if (!g_bauto || !have_time || !g_bmonth)
        return false;
    return local->tm_mon + 1 == g_bmonth && local->tm_mday == g_bday;
}

#define DOUBLE_MIN_US (1000 * 1000LL)
#define DOUBLE_MAX_US (10 * 1000 * 1000LL)
static int64_t g_last_sweep_us;

void net_on_sweep(void)
{
    int64_t now = esp_timer_get_time();
    bool have_time = time_ready();
    struct tm local = {0};
    if (have_time) {
        time_t wall = time(NULL);
        localtime_r(&wall, &local);
    }
    xSemaphoreTake(mu, portMAX_DELAY);
    // A detection less than 1 s after the last accepted sweep is the same
    // wave (or echo jitter), not a new sweep: ignore it completely.
    if (g_last_sweep_us && now - g_last_sweep_us < DOUBLE_MIN_US) {
        printf("SWEEP ignored: %lld ms after previous (<1000 ms)\n", (long long)((now - g_last_sweep_us) / 1000));
        xSemaphoreGive(mu);
        return;
    }
    g_last_sweep_us = now;
    ++g_anim_seq;
    if (birthday_now_locked(have_time, &local)) {
        printf("SWEEP single (birthday): blow candles, fireworks, wish\n");
        // Birthday screen: blow out the candles, then fireworks and a wish.
        // No API calls.
        ++g_blow_seq;
        g_candles_out_until_us = now + 60 * 1000000LL;
        g_last_fx_us = now;
        birthday_wish_locked();
        g_phrase_until_us = now + 9 * 1000000LL;
        g_arm_double = false;
        xSemaphoreGive(mu);
        return;
    }
    int64_t since_first = now - g_arm_us;
    if (g_arm_double && since_first >= DOUBLE_MIN_US && since_first <= DOUBLE_MAX_US) {
        // Second sweep 1-10 s after the first: show the balance in the bubble.
        g_arm_double = false;
        printf("SWEEP double: %lld ms after first -> balance\n", (long long)(since_first / 1000));
        if (!g_key[0]) {
            snprintf(g_phrase, sizeof(g_phrase), "密钥未设置");
        } else if (!g_connected) {
            snprintf(g_phrase, sizeof(g_phrase), "还没联网");
        } else if (queue_fetch(15 * 1000000LL)) {
            g_balance_bubble = true;
            snprintf(g_phrase, sizeof(g_phrase), "%s", PHRASE_BALANCE_WAIT);
        } else if (g_total[0]) {
            snprintf(g_phrase, sizeof(g_phrase), "%s", PHRASE_BALANCE);
        } else {
            snprintf(g_phrase, sizeof(g_phrase), "稍后再试");
        }
        g_phrase_until_us = now + 7 * 1000000LL;
    } else if (g_remind[0]) {
        printf("SWEEP single: dismiss reminder (sweep again in 1-10 s for balance)\n");
        dismiss_locked();
        g_arm_double = true;
        g_arm_us = now;
    } else {
        printf("SWEEP single: animation + quote (sweep again in 1-10 s for balance)\n");
        show_phrase_locked();
        g_arm_double = true;
        g_arm_us = now;
    }
    if (g_refill_pending && net_task_handle)
        xTaskNotifyGive(net_task_handle);
    xSemaphoreGive(mu);
}

void net_compose(UiState *ui)
{
    memset(ui, 0, sizeof(*ui));
    bool have_time = time_ready();
    struct tm local = {0};
    char clock_text[8] = {0};
    if (have_time) {
        time_t wall = time(NULL);
        localtime_r(&wall, &local);
        strftime(clock_text, sizeof(clock_text), "%H:%M", &local);
    }
    xSemaphoreTake(mu, portMAX_DELAY);
    ui->anim_seq = g_anim_seq;
    int64_t now_us = esp_timer_get_time();
    bool party_before = g_was_party;
    int phase = (int)((now_us / 4000000LL) % 2);
    (void)phase;
    if (g_phrase[0] && now_us > g_phrase_until_us)
        g_phrase[0] = 0;
    int countdown = have_time ? days_to_birthday_locked(&local) : -1;
    if (g_ap_active) {
        snprintf(ui->line[0], UI_COLS, "配网热点");
        snprintf(ui->line[1], UI_COLS, "%s", g_ap_ssid);
        snprintf(ui->line[2], UI_COLS, "密码 %s", g_ap_pass);
        snprintf(ui->line[3], UI_COLS, "%s", g_connected && g_ip[0] ? g_ip : "192.168.4.1");
    } else if (birthday_now_locked(have_time, &local)) {
        // Party screen: fireworks on entering (manual, auto on the date, or
        // BDAYDEMO) and every 4 minutes while idle; candles relight after 60 s.
        ui->party = true;
        if (!g_was_party || now_us - g_last_fx_us > 240 * 1000000LL) {
            ++g_fx_seq;
            g_last_fx_us = now_us;
        }
        ui->fx_seq = g_fx_seq;
        ui->blow_seq = g_blow_seq;
        ui->candles_out = now_us < g_candles_out_until_us;
        if (!strcmp(g_phrase, PHRASE_BALANCE))
            snprintf(ui->bubble, sizeof(ui->bubble), "余额\n%s%s", g_total, strcmp(g_currency, "CNY") ? g_currency : "元");
        else if (!strcmp(g_phrase, PHRASE_BALANCE_WAIT))
            snprintf(ui->bubble, sizeof(ui->bubble), "查余额中");
        else if (g_phrase[0])
            snprintf(ui->bubble, sizeof(ui->bubble), "%s", g_phrase);
        else
            snprintf(ui->bubble, sizeof(ui->bubble), "祝%s\n生日快乐\xf0\x9f\x8e\x82", g_bname[0] ? g_bname : "你");
    } else if (!strcmp(g_phrase, PHRASE_BALANCE) || !strcmp(g_phrase, PHRASE_BALANCE_WAIT)) {
        ui->bal_loading = g_phrase[1] == 'w';
        if (ui->bal_loading)
            snprintf(ui->bubble, sizeof(ui->bubble), "查余额中");
        else
            snprintf(ui->balance, sizeof(ui->balance), "%s", g_total);
        if (!strcmp(g_currency, "CNY") || !g_currency[0])
            snprintf(ui->bal_sign, sizeof(ui->bal_sign), "¥");
        else if (!strcmp(g_currency, "USD"))
            snprintf(ui->bal_sign, sizeof(ui->bal_sign), "$");
        else
            snprintf(ui->bal_sign, sizeof(ui->bal_sign), "%.7s", g_currency);
        ui->bal_ok = g_available;
        snprintf(ui->bal_time, sizeof(ui->bal_time), "%s", g_bal_hhmm);
    } else if (g_phrase[0]) {
        snprintf(ui->bubble, sizeof(ui->bubble), "%s", g_phrase);
    } else if (!g_ssid[0]) {
        snprintf(ui->line[0], UI_COLS, "你好呀");
        snprintf(ui->line[1], UI_COLS, "还没设置WiFi");
        snprintf(ui->line[2], UI_COLS, "稍后开配网热点");
        snprintf(ui->line[3], UI_COLS, "或用USB串口");
    } else if (!g_connected) {
        if (have_time)
            snprintf(ui->line[0], UI_COLS, "%s %s", clock_text, WEEKDAYS[local.tm_wday % 7]);
        else
            snprintf(ui->line[0], UI_COLS, "你好呀");
        snprintf(ui->line[1], UI_COLS, "%s", g_wifi);
        snprintf(ui->line[2], UI_COLS, "%s", g_ssid);
        if (g_remind[0])
            snprintf(ui->line[3], UI_COLS, "%s", g_remind);
    } else if (!have_time) {
        snprintf(ui->line[0], UI_COLS, "你好呀");
        snprintf(ui->line[1], UI_COLS, "%s", g_ip[0] ? g_ip : "等待地址");
        snprintf(ui->line[2], UI_COLS, "等待校时");
    } else {
        // Default companion screen: clock card with date and current weather.
        ui->home = true;
        strftime(ui->clock, sizeof(ui->clock), "%H:%M", &local);
        ui->colon = (local.tm_sec % 2) == 0;
        snprintf(ui->date, sizeof(ui->date), "%d/%d %s", local.tm_mon + 1, local.tm_mday,
                 WEEKDAYS[local.tm_wday % 7]);
        if (!g_city[0]) {
            snprintf(ui->cond, sizeof(ui->cond), "未设地点");
        } else if (g_wcode < 0) {
            snprintf(ui->cond, sizeof(ui->cond), "查询中");
        } else {
            snprintf(ui->temp, sizeof(ui->temp), "%d°", g_temp_c);
            snprintf(ui->cond, sizeof(ui->cond), "%s", weather_name(g_wcode));
            ui->icon = weather_icon(g_wcode);
        }
        // Optional pill: due reminder, then things the user turned on.
        // No birthday countdown on screen (only the day itself is celebrated).
        (void)countdown;
        if (g_remind[0])
            snprintf(ui->note, sizeof(ui->note), "%s", g_remind);
        else if (g_ip[0] && now_us < g_ip_until_us)
            snprintf(ui->note, sizeof(ui->note), "%s", g_ip);
        else if (g_msg[0])
            snprintf(ui->note, sizeof(ui->note), "%s", g_msg);
    }
    g_was_party = ui->party;
    (void)party_before;
    xSemaphoreGive(mu);
}

void net_snapshot(NetSnapshot *out)
{
    memset(out, 0, sizeof(*out));
    bool have_time = time_ready();
    struct tm local = {0};
    if (have_time) {
        time_t now = time(NULL);
        localtime_r(&now, &local);
    }
    xSemaphoreTake(mu, portMAX_DELAY);
    snprintf(out->ssid, sizeof(out->ssid), "%s", g_ssid);
    snprintf(out->city, sizeof(out->city), "%s", g_city);
    snprintf(out->weather, sizeof(out->weather), "%s", g_weather);
    snprintf(out->total, sizeof(out->total), "%s", g_total);
    snprintf(out->currency, sizeof(out->currency), "%s", g_currency);
    snprintf(out->ip, sizeof(out->ip), "%s", g_ip);
    snprintf(out->wifi, sizeof(out->wifi), "%s", g_wifi);
    snprintf(out->detail, sizeof(out->detail), "%s", g_detail);
    out->drink_min = g_drink_min;
    out->move_min = g_move_min;
    out->quiet_start = g_quiet_start;
    out->quiet_end = g_quiet_end;
    out->tz_hours = g_tz_hours;
    out->drinks_today = g_drinks_today;
    out->key_set = g_key[0] != 0;
    out->connected = g_connected;
    out->time_ok = have_time;
    if (have_time)
        strftime(out->clock, sizeof(out->clock), "%H:%M", &local);
    snprintf(out->place, sizeof(out->place), "%s", g_city);
    out->lat_e6 = g_lat_e6;
    out->lon_e6 = g_lon_e6;
    out->has_geo = g_has_geo;
    out->geo_fixed = g_geo_fixed;
    snprintf(out->wx_today, sizeof(out->wx_today), "%s", g_wx_today);
    snprintf(out->wx_tomorrow, sizeof(out->wx_tomorrow), "%s", g_wx_tomorrow);
    out->mode = g_mode;
    out->birthday_now = birthday_now_locked(have_time, &local);
    snprintf(out->bname, sizeof(out->bname), "%s", g_bname);
    out->bmonth = g_bmonth;
    out->bday = g_bday;
    out->bauto = g_bauto;
    out->days_to_birthday = have_time ? days_to_birthday_locked(&local) : -1;
    snprintf(out->msg, sizeof(out->msg), "%s", g_msg);
    out->rpause = g_rpause;
    out->llm_today = (have_time && g_llm_day == llm_day_stamp()) ? g_llm_count : 0;
    out->llm_cap = LLM_DAILY_CAP;
    out->ap_active = g_ap_active;
    snprintf(out->ap_ssid, sizeof(out->ap_ssid), "%s", g_ap_ssid);
    xSemaphoreGive(mu);
    out->heap_free = (unsigned)esp_get_free_heap_size();
    out->heap_min = (unsigned)esp_get_minimum_free_heap_size();
    out->uptime_s = (unsigned)(esp_timer_get_time() / 1000000LL);
    out->sweep_cm = sonar_threshold_cm();
}

bool net_set_location(int lat_e6, int lon_e6, const char *name)
{
    if (lat_e6 < -90000000 || lat_e6 > 90000000 || lon_e6 < -180000000 || lon_e6 > 180000000)
        return false;
    if (!name || !name[0])
        name = "定位点";
    if (strlen(name) >= sizeof(g_city))
        return false;
    char lat_text[16];
    char lon_text[16];
    xSemaphoreTake(mu, portMAX_DELAY);
    bool same = g_geo_fixed && g_lat_e6 == lat_e6 && g_lon_e6 == lon_e6 && !strcmp(g_city, name);
    if (!same) {
        copy_field(g_city, sizeof(g_city), name);
        g_lat_e6 = lat_e6;
        g_lon_e6 = lon_e6;
        g_has_geo = true;
        g_geo_fixed = true;
        g_weather[0] = 0;
        g_wx_today[0] = 0;
        g_wx_tomorrow[0] = 0;
        queue_weather();
    }
    xSemaphoreGive(mu);
    if (same)
        return true;
    snprintf(lat_text, sizeof(lat_text), "%d", lat_e6);
    snprintf(lon_text, sizeof(lon_text), "%d", lon_e6);
    nvs_put("city", name);
    nvs_put("lat", lat_text);
    nvs_put("lon", lon_text);
    save_int("geofix", 1);
    printf("saved\n");
    fflush(stdout);
    return true;
}

void net_set_mode(int mode)
{
    mode = mode == 1 ? 1 : 0;
    xSemaphoreTake(mu, portMAX_DELAY);
    bool changed = g_mode != mode;
    g_mode = mode;
    g_phrase[0] = 0;
    xSemaphoreGive(mu);
    if (changed) {
        save_int("mode", mode);
        printf("saved\n");
        fflush(stdout);
    }
}

bool net_set_birthday(const char *name, int month, int day, bool auto_switch)
{
    static const int days_in[] = {31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month || day) {
        if (month < 1 || month > 12 || day < 1 || day > days_in[month - 1])
            return false;
    }
    if (name && strlen(name) >= sizeof(g_bname))
        return false;
    xSemaphoreTake(mu, portMAX_DELAY);
    if (name)
        copy_field(g_bname, sizeof(g_bname), name);
    g_bmonth = month;
    g_bday = day;
    g_bauto = auto_switch;
    char saved_name[sizeof(g_bname)];
    copy_field(saved_name, sizeof(saved_name), g_bname);
    xSemaphoreGive(mu);
    nvs_put("bname", saved_name);
    save_int("bmon", month);
    save_int("bday", day);
    save_int("bauto", auto_switch ? 1 : 0);
    printf("saved\n");
    fflush(stdout);
    return true;
}

void net_set_message(const char *text)
{
    char clean[sizeof(g_msg)];
    snprintf(clean, sizeof(clean), "%s", text ? text : "");
    xSemaphoreTake(mu, portMAX_DELAY);
    bool changed = strcmp(clean, g_msg) != 0;
    copy_field(g_msg, sizeof(g_msg), clean);
    xSemaphoreGive(mu);
    if (changed) {
        nvs_put("msg", clean);
        printf("saved\n");
        fflush(stdout);
    }
}

void net_set_pause(bool paused)
{
    xSemaphoreTake(mu, portMAX_DELAY);
    bool changed = g_rpause != paused;
    g_rpause = paused;
    xSemaphoreGive(mu);
    if (changed) {
        save_int("rpause", paused ? 1 : 0);
        printf("saved\n");
        fflush(stdout);
    }
}

void net_apply_update(const NetUpdate *in)
{
    bool wifi = false;
    bool key_changed = false;
    bool open_net = false;
    bool save_ssid = false;
    bool save_pass = false;
    bool save_key = false;
    bool save_city = false;
    bool save_nums = false;
    bool tz_changed = false;
    char ssid[32] = {0};
    char pass[64] = {0};
    char apikey[128] = {0};
    char city[32] = {0};
    int drink = 0;
    int move = 0;
    int q0 = 0;
    int q1 = 0;
    int tz = 8;
    xSemaphoreTake(mu, portMAX_DELAY);
    if (in->has_ssid && strcmp(in->ssid, g_ssid) && strlen(in->ssid) < sizeof(g_ssid)) {
        copy_field(g_ssid, sizeof(g_ssid), in->ssid);
        copy_field(ssid, sizeof(ssid), g_ssid);
        save_ssid = wifi = true;
    }
    if (in->open_net) {
        g_pass[0] = 0;
        open_net = wifi = true;
    } else if (in->has_pass && in->pass[0]) {
        if (strlen(in->pass) < 8 || strlen(in->pass) >= sizeof(g_pass)) {
            detail("至少8位");
        } else {
            copy_field(g_pass, sizeof(g_pass), in->pass);
            copy_field(pass, sizeof(pass), g_pass);
            save_pass = wifi = true;
        }
    }
    if (in->clear_key) {
        g_key[0] = 0;
        g_available = false;
        g_refill_pending = false;
        save_key = key_changed = true;
    } else if (in->has_key && in->key[0]) {
        if (strlen(in->key) >= sizeof(g_key)) {
            detail("配置失败");
        } else {
            copy_field(g_key, sizeof(g_key), in->key);
            copy_field(apikey, sizeof(apikey), g_key);
            save_key = key_changed = true;
        }
    }
    if (in->has_city && strlen(in->city) < sizeof(g_city) &&
        (strcmp(in->city, g_city) || (in->geo_auto && g_geo_fixed))) {
        copy_field(g_city, sizeof(g_city), in->city);
        copy_field(city, sizeof(city), g_city);
        g_has_geo = false;
        g_geo_fixed = false;
        g_wcode = -1;
        g_wx_today[0] = 0;
        g_wx_tomorrow[0] = 0;
        g_lat_e6 = 0;
        g_lon_e6 = 0;
        g_weather[0] = 0;
        queue_weather();
        save_city = true;
    }
    if (in->has_nums) {
        g_drink_min = in->drink_min;
        g_move_min = in->move_min;
        g_quiet_start = in->quiet_start;
        g_quiet_end = in->quiet_end;
        if ((in->tz_hours == 8 || in->tz_hours == 9) && in->tz_hours != g_tz_hours) {
            g_tz_hours = in->tz_hours;
            tz_changed = true;
            queue_weather();
        }
        drink = g_drink_min;
        move = g_move_min;
        q0 = g_quiet_start;
        q1 = g_quiet_end;
        tz = g_tz_hours;
        save_nums = true;
        if (g_drink_min == 0 && g_remind_kind == 1) {
            g_remind[0] = 0;
            g_remind_kind = 0;
        }
        if (g_move_min == 0 && g_remind_kind == 2) {
            g_remind[0] = 0;
            g_remind_kind = 0;
        }
    }
    if (key_changed && !in->clear_key)
        queue_fetch(0);
    xSemaphoreGive(mu);

    if (save_ssid)
        nvs_put("ssid", ssid);
    if (open_net)
        nvs_put("pass", "");
    else if (save_pass)
        nvs_put("pass", pass);
    if (save_key)
        nvs_put("key", in->clear_key ? "" : apikey);
    if (save_city) {
        nvs_put("city", city);
        nvs_put("lat", "");
        nvs_put("lon", "");
        save_int("geofix", 0);
    }
    if (save_nums) {
        save_int("drink", drink);
        save_int("move", move);
        save_int("q0", q0);
        save_int("q1", q1);
        save_int("tz", tz);
    }
    if (tz_changed)
        apply_tz();
    if (wifi)
        request_reconnect();
    if (save_ssid || save_pass || save_key || save_city || save_nums || open_net) {
        printf("saved\n");
        fflush(stdout);
    }
}

void net_handle_line(char *line)
{
    NetUpdate update;
    memset(&update, 0, sizeof(update));
    if (!strncmp(line, "SSID ", 5) || !strcmp(line, "SSID")) {
        snprintf(update.ssid, sizeof(update.ssid), "%s", line[4] ? line + 5 : "");
        update.has_ssid = true;
        net_apply_update(&update);
    } else if (!strncmp(line, "PASS ", 5)) {
        snprintf(update.pass, sizeof(update.pass), "%s", line + 5);
        update.has_pass = true;
        net_apply_update(&update);
    } else if (!strcmp(line, "PASS")) {
        update.open_net = true;
        net_apply_update(&update);
    } else if (!strncmp(line, "KEY ", 4)) {
        snprintf(update.key, sizeof(update.key), "%s", line + 4);
        update.has_key = true;
        net_apply_update(&update);
    } else if (!strncmp(line, "CITY ", 5) || !strcmp(line, "CITY")) {
        snprintf(update.city, sizeof(update.city), "%s", line[4] ? line + 5 : "");
        update.has_city = true;
        net_apply_update(&update);
    } else if (!strncmp(line, "DRINK ", 6) || !strncmp(line, "MOVE ", 5) ||
               !strncmp(line, "QUIET ", 6) || !strncmp(line, "TZ ", 3)) {
        xSemaphoreTake(mu, portMAX_DELAY);
        update.drink_min = g_drink_min;
        update.move_min = g_move_min;
        update.quiet_start = g_quiet_start;
        update.quiet_end = g_quiet_end;
        update.tz_hours = g_tz_hours;
        xSemaphoreGive(mu);
        int a = 0;
        int b = 0;
        bool ok = false;
        if (sscanf(line, "DRINK %d", &a) == 1 && a >= 0 && a <= 240) {
            update.drink_min = a;
            ok = true;
        } else if (sscanf(line, "MOVE %d", &a) == 1 && a >= 0 && a <= 240) {
            update.move_min = a;
            ok = true;
        } else if (sscanf(line, "QUIET %d %d", &a, &b) == 2 && a >= 0 && a <= 23 && b >= 0 && b <= 23) {
            update.quiet_start = a;
            update.quiet_end = b;
            ok = true;
        } else if (sscanf(line, "TZ %d", &a) == 1 && (a == 8 || a == 9)) {
            update.tz_hours = a;
            ok = true;
        }
        if (!ok) {
            printf("bad value\n");
            fflush(stdout);
        } else {
            update.has_nums = true;
            net_apply_update(&update);
        }
    } else if (!strncmp(line, "LOC ", 4)) {
        char *end = 0;
        double lat = strtod(line + 4, &end);
        char *end2 = 0;
        double lon = end ? strtod(end, &end2) : 0;
        if (!end || end == line + 4 || !end2 || end2 == end) {
            printf("usage: LOC 28.6924 115.8153 新建区\n");
        } else {
            while (*end2 == ' ')
                ++end2;
            int lat_e6 = (int)(lat * 1000000.0 + (lat >= 0 ? 0.5 : -0.5));
            int lon_e6 = (int)(lon * 1000000.0 + (lon >= 0 ? 0.5 : -0.5));
            if (!net_set_location(lat_e6, lon_e6, end2))
                printf("bad value\n");
        }
        fflush(stdout);
    } else if (!strncmp(line, "MODE ", 5)) {
        const char *arg = line + 5;
        if (!strcmp(arg, "BDAY") || !strcmp(arg, "BIRTHDAY") || !strcmp(arg, "1") || !strcmp(arg, "生日"))
            net_set_mode(1);
        else if (!strcmp(arg, "PET") || !strcmp(arg, "COMPANION") || !strcmp(arg, "0") || !strcmp(arg, "陪伴"))
            net_set_mode(0);
        else
            printf("usage: MODE PET | MODE BDAY\n");
        fflush(stdout);
    } else if (!strncmp(line, "BNAME", 5) && (line[5] == ' ' || !line[5])) {
        int m, d;
        bool a;
        xSemaphoreTake(mu, portMAX_DELAY);
        m = g_bmonth;
        d = g_bday;
        a = g_bauto;
        xSemaphoreGive(mu);
        if (!net_set_birthday(line[5] ? line + 6 : "", m, d, a))
            printf("bad value\n");
        fflush(stdout);
    } else if (!strncmp(line, "BDAY", 4) && (line[4] == ' ' || !line[4])) {
        int m = 0, d = 0;
        bool a;
        xSemaphoreTake(mu, portMAX_DELAY);
        a = g_bauto;
        xSemaphoreGive(mu);
        bool off = !line[4] || !strcmp(line + 5, "OFF");
        if (!off && sscanf(line + 5, "%d %d", &m, &d) != 2 && sscanf(line + 5, "%d-%d", &m, &d) != 2) {
            printf("usage: BDAY 3 15 | BDAY OFF\n");
        } else if (!net_set_birthday(0, off ? 0 : m, off ? 0 : d, a)) {
            printf("bad value\n");
        }
        fflush(stdout);
    } else if (!strncmp(line, "BAUTO ", 6)) {
        int m, d;
        xSemaphoreTake(mu, portMAX_DELAY);
        m = g_bmonth;
        d = g_bday;
        xSemaphoreGive(mu);
        net_set_birthday(0, m, d, atoi(line + 6) == 1);
    } else if (!strncmp(line, "MSG", 3) && (line[3] == ' ' || !line[3])) {
        net_set_message(line[3] ? line + 4 : "");
        printf("%s\n", line[3] ? "message set" : "message cleared");
        fflush(stdout);
    } else if (!strncmp(line, "PAUSE ", 6)) {
        net_set_pause(atoi(line + 6) == 1);
        printf("reminders %s\n", atoi(line + 6) == 1 ? "paused" : "on");
        fflush(stdout);
    } else if (!strcmp(line, "AP") || !strcmp(line, "AP ON")) {
        xSemaphoreTake(mu, portMAX_DELAY);
        g_ap_request = true;
        xSemaphoreGive(mu);
        if (net_task_handle)
            xTaskNotifyGive(net_task_handle);
        printf("setup hotspot requested\n");
        fflush(stdout);
    } else if (!strcmp(line, "AP OFF")) {
        xSemaphoreTake(mu, portMAX_DELAY);
        g_ap_stop_request = true;
        g_ap_next_ok_us = esp_timer_get_time() + 30LL * 60 * 1000000LL;
        xSemaphoreGive(mu);
        if (net_task_handle)
            xTaskNotifyGive(net_task_handle);
        printf("setup hotspot off for 30 min\n");
        fflush(stdout);
    } else if (!strcmp(line, "PET")) {
        // Same as one hand wave (v0.1 had this command too).
        net_on_sweep();
        printf("pet\n");
        fflush(stdout);
    } else if (!strcmp(line, "BDAYDEMO") || !strcmp(line, "BDAYDEMO OFF")) {
        xSemaphoreTake(mu, portMAX_DELAY);
        bool on = !strcmp(line, "BDAYDEMO");
        g_demo_until_us = on ? esp_timer_get_time() + 180 * 1000000LL : 0;
        g_phrase[0] = 0;
        xSemaphoreGive(mu);
        printf(on ? "birthday demo 3 min: fireworks now, PET blows candles (date and mode unchanged)\n"
                  : "birthday demo off\n");
        fflush(stdout);
    } else if (!strcmp(line, "WEBTEST")) {
        portal_selftest();
    } else if (!strcmp(line, "POOL SELFTEST")) {
        pool_selftest();
        fflush(stdout);
    } else if (!strcmp(line, "REBOOT")) {
        pool_save();
        printf("rebooting (pool saved)\n");
        fflush(stdout);
        vTaskDelay(pdMS_TO_TICKS(200));
        esp_restart();
    } else if (!strcmp(line, "POOL") || !strcmp(line, "POOL REFILL") || !strcmp(line, "POOL LIST")) {
        xSemaphoreTake(mu, portMAX_DELAY);
        int pa, pu, pg;
        pool_counts_locked(&pa, &pu, &pg);
        printf("pool active %d unshown %d generated %d/%d refills %d\n", pa, pu, pg, POOL_GEN_MAX, g_pool.refills);
        if (!strcmp(line, "POOL LIST"))
            for (int id = 0; id < PET_QUOTE_COUNT + POOL_GEN_MAX; ++id)
                if (pool_active(id))
                    printf("  #%d [%s/%s] shows %d %s %s\n", id, T_NAMES[pool_time(id)], C_NAMES[pool_cond(id)],
                           id < PET_QUOTE_COUNT ? g_pool.bshows[id] : g_pool.gen[id - PET_QUOTE_COUNT].shows,
                           *pool_used(id) ? "used" : "new ", pool_text(id));
        bool refill = !strcmp(line, "POOL REFILL");
        if (refill) {
            if (!g_key[0] || !g_available) {
                printf("refill needs a working API key\n");
                refill = false;
            } else {
                // Manual: skips the 12 h spacing, still bound by the daily cap.
                g_refill_pending = true;
                g_refill_force = true;
                printf("refill queued\n");
            }
        }
        xSemaphoreGive(mu);
        if (refill && net_task_handle)
            xTaskNotifyGive(net_task_handle);
        fflush(stdout);
    } else if (!strncmp(line, "SWEEP ", 6)) {
        int cm = atoi(line + 6);
        printf("%s\n", sonar_set_threshold_cm(cm) ? "saved" : "bad value (5-200 cm)");
        fflush(stdout);
    } else if (!strncmp(line, "SONAR ", 6)) {
        sonar_set_debug(atoi(line + 6) == 1);
        printf("sonar debug %s\n", atoi(line + 6) == 1 ? "on" : "off");
        fflush(stdout);
    } else if (!strcmp(line, "FETCH")) {
        xSemaphoreTake(mu, portMAX_DELAY);
        bool queued = queue_fetch(3 * 1000000LL);
        xSemaphoreGive(mu);
        printf("%s\n", queued ? "fetch queued" : "busy");
        fflush(stdout);
    } else if (!strcmp(line, "STATUS")) {
        xSemaphoreTake(mu, portMAX_DELAY);
        printf("wifi %s ssid %s ip %s key %s\n",
               g_wifi, g_ssid[0] ? g_ssid : "-", g_ip[0] ? g_ip : "-", g_key[0] ? "set" : "empty");
        printf("balance %s total %s granted %s topped %s\n",
               g_currency[0] ? g_currency : "-",
               g_total[0] ? g_total : "-",
               g_granted[0] ? g_granted : "-",
               g_topped[0] ? g_topped : "-");
        printf("city %s weather %s tz %d drink %d move %d quiet %d-%d drinks %d\n",
               g_city[0] ? g_city : "-", g_weather[0] ? g_weather : "-", g_tz_hours,
               g_drink_min, g_move_min, g_quiet_start, g_quiet_end, g_drinks_today);
        {
            char la[16], lo[16];
            fmt_e6(la, sizeof(la), g_lat_e6);
            fmt_e6(lo, sizeof(lo), g_lon_e6);
            int64_t t_now = esp_timer_get_time();
            printf("weather refresh every 30 min (retry 2 min): last ok %llds ago, next in %llds\n",
                   g_weather_ok_us ? (long long)((t_now - g_weather_ok_us) / 1000000LL) : -1LL,
                   g_next_weather_us > t_now ? (long long)((g_next_weather_us - t_now) / 1000000LL) : 0LL);
            printf("loc %s lat %s lon %s %s today %s tomorrow %s\n", g_city[0] ? g_city : "-",
                   g_has_geo ? la : "-", g_has_geo ? lo : "-", g_geo_fixed ? "fixed" : "geocoded",
                   g_wx_today[0] ? g_wx_today : "-", g_wx_tomorrow[0] ? g_wx_tomorrow : "-");
        }
        printf("mode %s bname %s bday %02d-%02d bauto %d msg %s reminders %s\n",
               g_mode ? "birthday" : "companion", g_bname[0] ? g_bname : "-", g_bmonth, g_bday,
               g_bauto ? 1 : 0, g_msg[0] ? g_msg : "-", g_rpause ? "paused" : "on");
        {
            struct tm today = {0};
            bool have = time_ready();
            if (have) {
                time_t wall = time(NULL);
                localtime_r(&wall, &today);
            }
            int days = have ? days_to_birthday_locked(&today) : -1;
            printf("birthday %s, countdown %d day(s), screen %s\n", g_bmonth ? "set" : "unset", days,
                   birthday_now_locked(have, &today) ? "party" : "companion");
        }
        printf("llm %s today %d/%d (batch refills only, >=12 h apart)\n", LLM_MODEL,
               (time_ready() && g_llm_day == llm_day_stamp()) ? g_llm_count : 0, LLM_DAILY_CAP);
        int pa, pu, pg;
        pool_counts_locked(&pa, &pu, &pg);
        printf("pool active %d unshown %d generated %d/%d refills %d%s\n", pa, pu, pg, POOL_GEN_MAX,
               g_pool.refills, g_refill_pending ? " (refill queued)" : "");
        {
            int shows = 0;
            for (int id = 0; id < PET_QUOTE_COUNT + POOL_GEN_MAX; ++id)
                if (pool_active(id))
                    shows += id < PET_QUOTE_COUNT ? g_pool.bshows[id] : g_pool.gen[id - PET_QUOTE_COUNT].shows;
            PoolCtx ctx;
            context_now_locked(&ctx);
            char now[64];
            context_text(&ctx, now, sizeof(now));
            printf("pool shows %d (active lines) context now %s; loaded at boot: %s\n", shows, now, g_pool_boot);
        }
        {
            nvs_stats_t st = {0};
            nvs_get_stats(NULL, &st);
            printf("boot #%d reset %s; nvs entries used %u free %u\n", g_boots, g_reset_name,
                   (unsigned)st.used_entries, (unsigned)st.free_entries);
        }
        printf("detail %s heap %u min %u uptime %lus\n", g_detail, (unsigned)esp_get_free_heap_size(),
               (unsigned)esp_get_minimum_free_heap_size(), (unsigned long)(esp_timer_get_time() / 1000000LL));
        if (g_ap_active)
            printf("setup hotspot %s password %s http://192.168.4.1/ clients %d\n", g_ap_ssid, g_ap_pass,
                   g_ap_clients);
        else
            printf("setup hotspot off\n");
        {
            char sonar[96];
            sonar_status(sonar, sizeof(sonar));
            printf("%s\n", sonar);
        }
        printf("web http://pet.local/ or http://%s/ (same LAN); USB serial settings enabled\n",
               g_ip[0] ? g_ip : "no-IP-yet");
        xSemaphoreGive(mu);
        fflush(stdout);
    } else if (!strcmp(line, "SCAN")) {
        start_scan_wifi();
    } else if (!strcmp(line, "FORGET")) {
        nvs_handle_t handle;
        if (nvs_open(NVS_NS, NVS_READWRITE, &handle) == ESP_OK) {
            nvs_erase_key(handle, "ssid");
            nvs_erase_key(handle, "pass");
            nvs_erase_key(handle, "key");
            nvs_commit(handle);
            nvs_close(handle);
        }
        xSemaphoreTake(mu, portMAX_DELAY);
        g_ssid[0] = 0;
        g_pass[0] = 0;
        g_key[0] = 0;
        g_total[0] = 0;
        g_granted[0] = 0;
        g_topped[0] = 0;
        g_currency[0] = 0;
        g_available = false;
        g_refill_pending = false;
        detail("密钥未设置");
        xSemaphoreGive(mu);
        request_reconnect();
        printf("cleared\n");
        fflush(stdout);
    } else if (line[0]) {
        printf("commands: SSID, PASS, KEY, CITY, LOC, DRINK, MOVE, QUIET, TZ, MODE, BNAME, BDAY, BAUTO, MSG, PAUSE, SWEEP, SONAR, PET, POOL, BDAYDEMO, AP, FETCH, STATUS, SCAN, FORGET, WEBTEST\n");
        fflush(stdout);
    }
}
