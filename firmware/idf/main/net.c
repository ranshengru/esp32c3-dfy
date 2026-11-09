#include "net.h"
#include "portal.h"
#include "quotes.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_sntp.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "net";
static const char *NVS_NS = "pet";
static const char *BALANCE_URL = "https://api.deepseek.com/user/balance";

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
static char g_phrase[40];
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
static int64_t g_remind_until_us;
static int64_t g_last_drink_us;
static int64_t g_last_move_us;
static int64_t g_next_weather_us;
static int64_t g_arm_us;
static bool g_arm_double;
static char g_api_line[32];
static bool g_quote_pending;
static int64_t g_next_quote_us;
static wifi_ap_record_t g_scan_records[24];

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
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
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

static char body[3072];
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

static bool junk_codepoint(uint32_t codepoint)
{
    if (codepoint <= 32)
        return true;
    switch (codepoint) {
    case '"':
    case '\'':
    case '(':
    case ')':
    case '~':
    case 0x3002:
    case 0xff0c:
    case 0xff01:
    case 0xff1f:
    case 0x3001:
    case 0x2026:
    case 0x201c:
    case 0x201d:
    case 0x2018:
    case 0x2019:
    case 0xff08:
    case 0xff09:
    case 0xff5e:
        return true;
    default:
        return false;
    }
}

static void clean_quote(char *text)
{
    char cleaned[32];
    int used = 0;
    const unsigned char *cursor = (const unsigned char *)text;
    uint32_t codepoint;
    while (utf8_next(&cursor, &codepoint)) {
        if (junk_codepoint(codepoint))
            continue;
        int bytes = codepoint < 0x80 ? 1 : codepoint < 0x800 ? 2 : 3;
        if (used + bytes >= (int)sizeof(cleaned))
            break;
        if (codepoint < 0x80) {
            cleaned[used++] = (char)codepoint;
        } else if (codepoint < 0x800) {
            cleaned[used++] = (char)(0xc0 | (codepoint >> 6));
            cleaned[used++] = (char)(0x80 | (codepoint & 0x3f));
        } else {
            cleaned[used++] = (char)(0xe0 | (codepoint >> 12));
            cleaned[used++] = (char)(0x80 | ((codepoint >> 6) & 0x3f));
            cleaned[used++] = (char)(0x80 | (codepoint & 0x3f));
        }
    }
    cleaned[used] = 0;
    snprintf(text, 32, "%s", cleaned);
}

static bool quote_ok(const char *text)
{
    const unsigned char *cursor = (const unsigned char *)text;
    uint32_t codepoint;
    int count = 0;
    while (utf8_next(&cursor, &codepoint)) {
        if (junk_codepoint(codepoint) || codepoint > 0xffff || !display_has_glyph(codepoint))
            return false;
        if (++count > 7)
            return false;
    }
    return count >= 4;
}

static void show_phrase_locked(void)
{
    const char *line = PET_QUOTES[esp_random() % PET_QUOTE_COUNT];
    if (g_api_line[0] && (esp_random() % 5) == 0)
        line = g_api_line;
    snprintf(g_phrase, sizeof(g_phrase), "%s", line);
    g_phrase_until_us = esp_timer_get_time() + 8 * 1000000LL;
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
    if (!g_remind[0] && !in_quiet(local.tm_hour)) {
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
            detail(g_available ? "" : "不可用");
            ESP_LOGI(TAG, "balance %s %s available=%d", g_currency, g_total, g_available);
        }
        cJSON_Delete(root);
    }
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
    has_geo = g_has_geo;
    xSemaphoreGive(mu);
    int64_t delay = 10LL * 60 * 1000000LL;
    if (!city[0])
        delay = 30LL * 60 * 1000000LL;
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
            snprintf(line, sizeof(line), "%s %d度", weather_name(wcode), rounded);
            set_weather(line);
            delay = 30LL * 60 * 1000000LL;
            ESP_LOGI(TAG, "weather %s", line);
        }
        cJSON_Delete(root);
    }
    xSemaphoreTake(mu, portMAX_DELAY);
    g_next_weather_us = esp_timer_get_time() + delay;
    xSemaphoreGive(mu);
}

static void fetch_quote(const char *key)
{
    static const char *payload =
        "{\"model\":\"deepseek-chat\",\"temperature\":1.2,\"max_tokens\":24,"
        "\"messages\":[{\"role\":\"system\",\"content\":"
        "\"你是桌宠大肥鱼，圆、懒、贪吃、黏人。只回一句中文，4到7个字，不要标点不要引号不要表情。\"},"
        "{\"role\":\"user\",\"content\":\"说一句\"}]}";
    int status = https_call("https://api.deepseek.com/chat/completions", key, payload);
    int64_t delay = 60LL * 60 * 1000000LL;
    if (status == 200) {
        cJSON *root = cJSON_ParseWithLength(body, (size_t)body_len);
        cJSON *choices = root ? cJSON_GetObjectItem(root, "choices") : 0;
        cJSON *first = cJSON_GetArrayItem(choices, 0);
        cJSON *message = first ? cJSON_GetObjectItem(first, "message") : 0;
        cJSON *content = message ? cJSON_GetObjectItem(message, "content") : 0;
        char line[32] = {0};
        if (cJSON_IsString(content))
            snprintf(line, sizeof(line), "%s", content->valuestring);
        cJSON_Delete(root);
        clean_quote(line);
        if (quote_ok(line)) {
            delay = 6LL * 3600 * 1000000LL;
            xSemaphoreTake(mu, portMAX_DELAY);
            snprintf(g_api_line, sizeof(g_api_line), "%s", line);
            snprintf(g_phrase, sizeof(g_phrase), "%s", line);
            g_phrase_until_us = esp_timer_get_time() + 8 * 1000000LL;
            xSemaphoreGive(mu);
            nvs_put("line", line);
            ESP_LOGI(TAG, "quote %s", line);
        } else {
            ESP_LOGI(TAG, "quote rejected");
        }
    } else if (status == 401 || status == 402) {
        xSemaphoreTake(mu, portMAX_DELAY);
        g_available = false;
        detail(status == 401 ? "密钥无效" : "不可用");
        xSemaphoreGive(mu);
        delay = 6LL * 3600 * 1000000LL;
    }
    xSemaphoreTake(mu, portMAX_DELAY);
    g_quote_pending = false;
    g_next_quote_us = esp_timer_get_time() + delay;
    xSemaphoreGive(mu);
}

static void net_task(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
        check_reminders();
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
        quote = g_quote_pending;
        memcpy(key, g_key, sizeof(key));
        memcpy(ssid, g_ssid, sizeof(ssid));
        int64_t now = esp_timer_get_time();
        retry = !connected && !g_scan_in_progress && ssid[0] && g_retry_at_us && now >= g_retry_at_us;
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
            fetch_quote(key);
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
    (void)PET_GLYPH_PAD[0];
    mu = xSemaphoreCreateMutex();
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }
    nvs_load("ssid", g_ssid, sizeof(g_ssid));
    nvs_load("pass", g_pass, sizeof(g_pass));
    nvs_load("key", g_key, sizeof(g_key));
    nvs_load("city", g_city, sizeof(g_city));
    nvs_load("line", g_api_line, sizeof(g_api_line));
    if (!quote_ok(g_api_line))
        g_api_line[0] = 0;
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
        g_lat_e6 = atoi(lat_text);
        g_lon_e6 = atoi(lon_text);
        g_has_geo = true;
    }
    g_last_drink_us = esp_timer_get_time();
    g_last_move_us = g_last_drink_us;
    label(g_ssid[0] ? "连接中" : "未设置");
    detail("");

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
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
    portal_start();
    xTaskCreate(net_task, "net", 20480, 0, 5, &net_task_handle);
    printf("PET READY v0.4\n");
    printf("Open http://<device-ip>/ from the same home network; USB serial settings also work.\n");
    printf("SSID PASS KEY CITY DRINK MOVE QUIET TZ FETCH STATUS SCAN FORGET\n");
    fflush(stdout);
}

void net_request_fetch(void)
{
    xSemaphoreTake(mu, portMAX_DELAY);
    queue_fetch(15 * 1000000LL);
    xSemaphoreGive(mu);
}

void net_on_sweep(void)
{
    int64_t now = esp_timer_get_time();
    xSemaphoreTake(mu, portMAX_DELAY);
    if (g_arm_double && now - g_arm_us < 2500000) {
        g_arm_double = false;
        g_phrase[0] = 0;
        if (queue_fetch(15 * 1000000LL))
            detail("查询中");
        else
            detail("稍后再试");
    } else if (g_remind[0]) {
        dismiss_locked();
        g_arm_double = true;
        g_arm_us = now;
    } else {
        show_phrase_locked();
        g_arm_double = true;
        g_arm_us = now;
        if (g_key[0] && g_available && !g_quote_pending && now >= g_next_quote_us)
            g_quote_pending = true;
    }
    if (g_quote_pending && net_task_handle)
        xTaskNotifyGive(net_task_handle);
    xSemaphoreGive(mu);
}

void net_compose(UiState *ui)
{
    memset(ui, 0, sizeof(*ui));
    xSemaphoreTake(mu, portMAX_DELAY);
    int64_t now_us = esp_timer_get_time();
    if (g_phrase[0] && now_us > g_phrase_until_us)
        g_phrase[0] = 0;
    if (!g_ssid[0]) {
        snprintf(ui->line[0], UI_COLS, "小粤");
        snprintf(ui->line[1], UI_COLS, "请用USB连接");
        snprintf(ui->line[2], UI_COLS, "串口设置WiFi");
        snprintf(ui->line[3], UI_COLS, "SSID和密码");
    } else if (!g_connected) {
        snprintf(ui->line[0], UI_COLS, "小粤");
        snprintf(ui->line[1], UI_COLS, "%s", g_wifi);
        snprintf(ui->line[2], UI_COLS, "%s", g_ssid);
        snprintf(ui->line[3], UI_COLS, "USB串口可改设置");
    } else if (!time_ready()) {
        snprintf(ui->line[0], UI_COLS, "小粤");
        snprintf(ui->line[1], UI_COLS, "%s", g_ip[0] ? g_ip : "等待地址");
        snprintf(ui->line[2], UI_COLS, "等待校时");
        snprintf(ui->line[3], UI_COLS, "%s", g_detail[0] ? g_detail : "连扫查余额");
    } else {
        char clock_text[8];
        time_t now = time(NULL);
        struct tm local;
        localtime_r(&now, &local);
        strftime(clock_text, sizeof(clock_text), "%H:%M", &local);
        snprintf(ui->line[0], UI_COLS, "小粤 %s", clock_text);
        if (!g_city[0])
            snprintf(ui->line[1], UI_COLS, "未设城市");
        else if (!g_weather[0])
            snprintf(ui->line[1], UI_COLS, "天气查询中");
        else
            snprintf(ui->line[1], UI_COLS, "%s", g_weather);
        if (g_key[0])
            snprintf(ui->line[2], UI_COLS, "余额 %s %s", g_total[0] ? g_total : "--", g_currency);
        else
            snprintf(ui->line[2], UI_COLS, "密钥未设置");
        if (g_remind[0])
            snprintf(ui->line[3], UI_COLS, "%s", g_remind);
        else if (g_phrase[0])
            snprintf(ui->line[3], UI_COLS, "%s", g_phrase);
        else if (g_detail[0])
            snprintf(ui->line[3], UI_COLS, "%s", g_detail);
        else
            snprintf(ui->line[3], UI_COLS, "%s", g_key[0] ? "连扫查余额" : "扫一下说话");
    }
    xSemaphoreGive(mu);
}

void net_snapshot(NetSnapshot *out)
{
    memset(out, 0, sizeof(*out));
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
    out->time_ok = time_ready();
    if (out->time_ok) {
        time_t now = time(NULL);
        struct tm local;
        localtime_r(&now, &local);
        strftime(out->clock, sizeof(out->clock), "%H:%M", &local);
    }
    xSemaphoreGive(mu);
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
        g_quote_pending = false;
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
    if (in->has_city && strcmp(in->city, g_city) && strlen(in->city) < sizeof(g_city)) {
        copy_field(g_city, sizeof(g_city), in->city);
        copy_field(city, sizeof(city), g_city);
        g_has_geo = false;
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
        printf("quote %s detail %s heap %u\n",
               g_api_line[0] ? g_api_line : "-", g_detail, (unsigned)esp_get_free_heap_size());
        printf("web http://%s/ (same LAN); USB serial settings enabled\n",
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
        g_quote_pending = false;
        detail("密钥未设置");
        xSemaphoreGive(mu);
        request_reconnect();
        printf("cleared\n");
        fflush(stdout);
    } else if (line[0]) {
        printf("commands: SSID, PASS, KEY, CITY, DRINK, MOVE, QUIET, TZ, FETCH, STATUS, SCAN, FORGET\n");
        fflush(stdout);
    }
}
