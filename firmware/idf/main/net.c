#include "net.h"

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
static bool g_available;
static bool g_fetch_pending;
static int64_t g_last_fetch_us;
static int64_t g_retry_at_us;

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

static void queue_fetch(int64_t min_gap_us)
{
    int64_t now = esp_timer_get_time();
    if (g_last_fetch_us && now - g_last_fetch_us < min_gap_us)
        return;
    g_last_fetch_us = now;
    g_fetch_pending = true;
    if (net_task_handle)
        xTaskNotifyGive(net_task_handle);
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

static char body[1536];
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

static void fetch_balance(const char *key)
{
    if (!key[0]) {
        xSemaphoreTake(mu, portMAX_DELAY);
        detail("密钥未设置");
        xSemaphoreGive(mu);
        return;
    }

    xSemaphoreTake(mu, portMAX_DELAY);
    detail("查询中");
    xSemaphoreGive(mu);

    char auth[160];
    snprintf(auth, sizeof(auth), "Bearer %s", key);
    body_len = 0;
    body[0] = 0;
    esp_http_client_config_t config = {
        .url = BALANCE_URL,
        .method = HTTP_METHOD_GET,
        .timeout_ms = 10000,
        .event_handler = http_event,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 1024,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        xSemaphoreTake(mu, portMAX_DELAY);
        detail("查询失败");
        xSemaphoreGive(mu);
        return;
    }
    esp_http_client_set_header(client, "Authorization", auth);
    esp_http_client_set_header(client, "Accept", "application/json");
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    xSemaphoreTake(mu, portMAX_DELAY);
    if (err != ESP_OK) {
        detail("查询失败");
        ESP_LOGW(TAG, "balance request failed: %s", esp_err_to_name(err));
    } else if (status == 401) {
        detail("密钥无效");
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
            detail(g_available ? "扫一下刷新" : "不可用");
            ESP_LOGI(TAG, "balance %s %s available=%d", g_currency, g_total, g_available);
        }
        cJSON_Delete(root);
    }
    xSemaphoreGive(mu);
}

static void net_task(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
        bool connected;
        bool pending;
        bool retry;
        char key[128];
        char ssid[sizeof(g_ssid)];
        xSemaphoreTake(mu, portMAX_DELAY);
        connected = g_connected;
        pending = g_fetch_pending;
        memcpy(key, g_key, sizeof(key));
        memcpy(ssid, g_ssid, sizeof(ssid));
        retry = !connected && ssid[0] && g_retry_at_us && esp_timer_get_time() >= g_retry_at_us;
        if (retry) {
            g_retry_at_us = esp_timer_get_time() + 8000000;
            label("连接中");
        }
        if (connected && pending)
            g_fetch_pending = false;
        xSemaphoreGive(mu);
        if (retry)
            start_connect();
        if (!connected || !pending)
            continue;
        if (!time_ready()) {
            ensure_sntp();
            xSemaphoreTake(mu, portMAX_DELAY);
            g_fetch_pending = true;
            detail("等待校时");
            xSemaphoreGive(mu);
            continue;
        }
        fetch_balance(key);
    }
}

static void copy_field(char *dest, size_t dest_len, const char *src)
{
    snprintf(dest, dest_len, "%s", src);
}

void net_init(void)
{
    setenv("TZ", "CST-8", 1);
    tzset();
    mu = xSemaphoreCreateMutex();
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }
    nvs_load("ssid", g_ssid, sizeof(g_ssid));
    nvs_load("pass", g_pass, sizeof(g_pass));
    nvs_load("key", g_key, sizeof(g_key));
    label(g_ssid[0] ? "连接中" : "未设置");
    detail(g_key[0] ? "扫一下刷新" : "密钥未设置");

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, 0));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, 0));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    if (g_ssid[0]) {
        wifi_config_t cfg;
        fill_sta_config(&cfg, g_ssid, g_pass);
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
    }
    ESP_ERROR_CHECK(esp_wifi_start());

    xTaskCreate(net_task, "net", 16384, 0, 5, &net_task_handle);
    printf("PET READY net\n");
    printf("SSID <name>\nPASS <secret>\nKEY <api-key>\nFETCH\nSTATUS\nFORGET\n");
    fflush(stdout);
}

void net_request_fetch(void)
{
    xSemaphoreTake(mu, portMAX_DELAY);
    queue_fetch(15 * 1000000LL);
    xSemaphoreGive(mu);
}

void net_compose(UiState *ui)
{
    memset(ui, 0, sizeof(*ui));
    xSemaphoreTake(mu, portMAX_DELAY);
    snprintf(ui->line[0], UI_COLS, "小粤");
    if (!g_ssid[0]) {
        snprintf(ui->line[1], UI_COLS, "未设置网络");
        snprintf(ui->line[2], UI_COLS, "SSID 网络名");
        snprintf(ui->line[3], UI_COLS, "KEY 密钥");
    } else if (!g_connected) {
        snprintf(ui->line[1], UI_COLS, "%s", g_wifi);
        snprintf(ui->line[2], UI_COLS, "%s", g_ssid);
        snprintf(ui->line[3], UI_COLS, "%s", g_detail);
    } else if (time_ready()) {
        char clock_text[8];
        time_t now = time(NULL);
        struct tm local;
        localtime_r(&now, &local);
        strftime(clock_text, sizeof(clock_text), "%H:%M", &local);
        snprintf(ui->line[1], UI_COLS, "%s %s", g_wifi, clock_text);
        if (g_key[0])
            snprintf(ui->line[2], UI_COLS, "余额 %s %s", g_total[0] ? g_total : "--", g_currency);
        else
            snprintf(ui->line[2], UI_COLS, "密钥未设置");
        snprintf(ui->line[3], UI_COLS, "%s", g_detail[0] ? g_detail : "扫一下刷新");
    } else {
        snprintf(ui->line[1], UI_COLS, "%s", g_ip[0] ? g_ip : "等待地址");
        snprintf(ui->line[2], UI_COLS, "等待校时");
        snprintf(ui->line[3], UI_COLS, "%s", g_detail);
    }
    xSemaphoreGive(mu);
}

static bool save_value(const char *nvs_key, char *dest, size_t dest_len, const char *value, const char *what)
{
    if (strlen(value) >= dest_len) {
        printf("%s too long\n", what);
        fflush(stdout);
        return false;
    }
    xSemaphoreTake(mu, portMAX_DELAY);
    copy_field(dest, dest_len, value);
    xSemaphoreGive(mu);
    esp_err_t err = nvs_put(nvs_key, value);
    if (err != ESP_OK) {
        printf("save %s failed: %s\n", what, esp_err_to_name(err));
        fflush(stdout);
        return false;
    }
    printf("%s saved\n", what);
    fflush(stdout);
    return true;
}

void net_handle_line(char *line)
{
    if (!strncmp(line, "SSID ", 5)) {
        if (save_value("ssid", g_ssid, sizeof(g_ssid), line + 5, "ssid"))
            request_reconnect();
    } else if (!strncmp(line, "PASS ", 5)) {
        if (save_value("pass", g_pass, sizeof(g_pass), line + 5, "pass"))
            request_reconnect();
    } else if (!strcmp(line, "PASS")) {
        if (save_value("pass", g_pass, sizeof(g_pass), "", "pass"))
            request_reconnect();
    } else if (!strncmp(line, "KEY ", 4)) {
        if (save_value("key", g_key, sizeof(g_key), line + 4, "key")) {
            xSemaphoreTake(mu, portMAX_DELAY);
            queue_fetch(0);
            xSemaphoreGive(mu);
        }
    } else if (!strcmp(line, "FETCH")) {
        xSemaphoreTake(mu, portMAX_DELAY);
        queue_fetch(3 * 1000000LL);
        xSemaphoreGive(mu);
        printf("fetch queued\n");
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
        printf("detail %s heap %u\n", g_detail, (unsigned)esp_get_free_heap_size());
        xSemaphoreGive(mu);
        fflush(stdout);
    } else if (!strcmp(line, "FORGET")) {
        nvs_handle_t handle;
        if (nvs_open(NVS_NS, NVS_READWRITE, &handle) == ESP_OK) {
            nvs_erase_all(handle);
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
        detail("密钥未设置");
        xSemaphoreGive(mu);
        request_reconnect();
        printf("cleared\n");
        fflush(stdout);
    } else if (line[0]) {
        printf("commands: SSID, PASS, KEY, FETCH, STATUS, FORGET\n");
        fflush(stdout);
    }
}
