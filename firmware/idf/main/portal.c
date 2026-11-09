#include "portal.h"

#include "net.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include <unistd.h>

static const char *TAG = "portal";
static SemaphoreHandle_t page_mu;
static char page[8192];

static int hex_nibble(int c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static void url_decode(char *text)
{
    char *write = text;
    for (char *read = text; *read; ) {
        if (*read == '+') {
            *write++ = ' ';
            ++read;
        } else if (*read == '%' && hex_nibble((unsigned char)read[1]) >= 0 &&
                   hex_nibble((unsigned char)read[2]) >= 0) {
            *write++ = (char)((hex_nibble((unsigned char)read[1]) << 4) |
                              hex_nibble((unsigned char)read[2]));
            read += 3;
        } else {
            *write++ = *read++;
        }
    }
    *write = 0;
}

typedef struct {
    char *cursor;
    int left;
    int used;
} Buf;

static void add(Buf *buf, const char *text)
{
    int length = (int)strlen(text);
    if (length > buf->left)
        length = buf->left;
    if (length > 0)
        memcpy(buf->cursor, text, (size_t)length);
    buf->cursor += length;
    buf->left -= length;
    buf->used += length;
}

static void add_escape(Buf *buf, const char *text)
{
    char amp[] = {'&', 'a', 'm', 'p', ';', 0};
    char lt[] = {'&', 'l', 't', ';', 0};
    char gt[] = {'&', 'g', 't', ';', 0};
    char quot[] = {'&', 'q', 'u', 'o', 't', ';', 0};
    for (; *text; ++text) {
        const char *rep = 0;
        if (*text == '&')
            rep = amp;
        else if (*text == '<')
            rep = lt;
        else if (*text == '>')
            rep = gt;
        else if (*text == '"')
            rep = quot;
        if (rep)
            add(buf, rep);
        else if ((unsigned char)*text >= 32 && buf->left > 0) {
            *buf->cursor++ = *text;
            --buf->left;
            ++buf->used;
        }
    }
}

static int clamp_int(const char *text, int fallback, int min, int max)
{
    if (!text || !text[0])
        return fallback;
    char *end = 0;
    long value = strtol(text, &end, 10);
    if (end == text || value < min || value > max)
        return fallback;
    return (int)value;
}

static int build_page(char *dest, int dest_len)
{
    NetSnapshot snap;
    net_snapshot(&snap);
    Buf buf = {.cursor = dest, .left = dest_len - 1, .used = 0};
    add(&buf,
        "<!DOCTYPE html><html lang=\"zh\"><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<title>小粤</title><style>"
        "body{margin:0;background:#1c1916;color:#f3eadc;font:16px/1.45 sans-serif}"
        "main{max-width:28rem;margin:0 auto;padding:1.1rem}"
        "h1{font-size:1.45rem;margin:0}"
        "p{color:#b7aa98}"
        ".card{background:#2a2622;border-radius:14px;padding:.85rem 1rem;margin:.7rem 0}"
        "label{display:block;margin:.85rem 0 .25rem}"
        "label.row{display:flex;align-items:center}"
        "input,select{width:100%;box-sizing:border-box;padding:.7rem;border-radius:10px;"
        "border:1px solid #4a433a;background:#1c1916;color:#f3eadc;font-size:16px}"
        "input[type=checkbox]{width:auto;margin-right:.45rem}"
        "button{width:100%;margin-top:1.1rem;padding:.85rem;border:0;border-radius:12px;"
        "background:#f3eadc;color:#1c1916;font-size:16px}"
        "</style></head><body><main><h1>小粤</h1>"
        "<p>连上热点就能改。密码和密钥留空表示不修改。</p><div class=\"card\">");

    add(&buf, "网络 ");
    add_escape(&buf, snap.connected ? "已连接 " : snap.wifi);
    if (snap.connected && snap.ip[0]) {
        add(&buf, " ");
        add_escape(&buf, snap.ip);
    }
    add(&buf, "<br>时间 ");
    add_escape(&buf, snap.time_ok ? snap.clock : "未校时");
    add(&buf, "<br>天气 ");
    if (!snap.city[0])
        add(&buf, "未设城市");
    else if (!snap.weather[0])
        add(&buf, "天气查询中");
    else
        add_escape(&buf, snap.weather);
    add(&buf, "<br>余额 ");
    if (!snap.key_set)
        add(&buf, "未设置密钥");
    else if (!snap.total[0])
        add(&buf, "还没有");
    else {
        add_escape(&buf, snap.total);
        add(&buf, " ");
        add_escape(&buf, snap.currency);
    }
    char count[160];
    snprintf(count, sizeof(count), "<br>今天喝水 %d 次", snap.drinks_today);
    add(&buf, count);
    if (snap.detail[0]) {
        add(&buf, "<br>");
        add_escape(&buf, snap.detail);
    }
    add(&buf, "</div><form method=\"post\" action=\"/save\">");
    add(&buf, "<label>Wi-Fi 名称</label><input name=\"ssid\" maxlength=\"31\" value=\"");
    add_escape(&buf, snap.ssid);
    add(&buf, "\"><label>Wi-Fi 密码</label><input name=\"pass\" type=\"password\" maxlength=\"63\" autocomplete=\"off\" placeholder=\"留空不改\">");
    add(&buf, "<label class=\"row\"><input type=\"checkbox\" name=\"open\" value=\"1\">开放网络，不使用密码</label>");
    add(&buf, "<label>DeepSeek 密钥</label><input name=\"key\" type=\"password\" maxlength=\"127\" autocomplete=\"off\" placeholder=\"");
    add(&buf, snap.key_set ? "已设置，留空不改" : "未设置");
    add(&buf, "\"><label class=\"row\"><input type=\"checkbox\" name=\"clear\" value=\"1\">清除密钥</label>");
    add(&buf, "<label>城市，用来查天气</label><input name=\"city\" maxlength=\"31\" value=\"");
    add_escape(&buf, snap.city);
    add(&buf, "\" placeholder=\"北京 或 东京\">");
    add(&buf, "<label>时区</label><select name=\"tz\">");
    add(&buf, snap.tz_hours == 9
                 ? "<option value=\"8\">北京时间</option><option value=\"9\" selected>东京时间</option>"
                 : "<option value=\"8\" selected>北京时间</option><option value=\"9\">东京时间</option>");
    snprintf(count, sizeof(count),
             "<label>喝水间隔，分钟，0 关闭</label><input name=\"drink\" type=\"number\" min=\"0\" max=\"240\" value=\"%d\">",
             snap.drink_min);
    add(&buf, count);
    snprintf(count, sizeof(count),
             "<label>久坐间隔，分钟，0 关闭</label><input name=\"move\" type=\"number\" min=\"0\" max=\"240\" value=\"%d\">",
             snap.move_min);
    add(&buf, count);
    snprintf(count, sizeof(count),
             "<label>勿扰开始，点</label><input name=\"q0\" type=\"number\" min=\"0\" max=\"23\" value=\"%d\">",
             snap.quiet_start);
    add(&buf, count);
    snprintf(count, sizeof(count),
             "<label>勿扰结束，点</label><input name=\"q1\" type=\"number\" min=\"0\" max=\"23\" value=\"%d\">",
             snap.quiet_end);
    add(&buf, count);
    add(&buf, "<button type=\"submit\">保存</button></form>");
    add(&buf, "<p>勿扰从开始点到结束点，跨过午夜也可以，开始和结束相同就没有勿扰。"
              "扫一下是说话，或者关掉当前提醒；连续扫两次才查余额。"
              "余额够的时候，大约六小时会向接口要一句新话，字不够就不用。"
              "热点名 " PET_AP_SSID "，密码 " PET_AP_PASS "，地址 http://192.168.4.1 。"
              "已经连上家里的网时，用串口 STATUS 里的 IP 也能打开。</p></main></body></html>");
    if (buf.left <= 0)
        ESP_LOGW(TAG, "settings page truncated");
    dest[buf.used] = 0;
    return buf.used;
}

static esp_err_t root_get(httpd_req_t *req)
{
    xSemaphoreTake(page_mu, portMAX_DELAY);
    int length = build_page(page, (int)sizeof(page));
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_send(req, page, length);
    xSemaphoreGive(page_mu);
    return err;
}

static esp_err_t save_post(httpd_req_t *req)
{
    if (req->content_len > 1024) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "form too large");
        return ESP_FAIL;
    }
    char body[1025];
    int got = 0;
    while (got < req->content_len) {
        int n = httpd_req_recv(req, body + got, (size_t)(req->content_len - got));
        if (n <= 0)
            return ESP_FAIL;
        got += n;
    }
    body[got] = 0;

    char ssid[32] = {0};
    char pass[64] = {0};
    char key[128] = {0};
    char city[32] = {0};
    char drink[8] = {0};
    char move[8] = {0};
    char q0[8] = {0};
    char q1[8] = {0};
    char tz[8] = {0};
    bool saw_ssid = false, saw_pass = false, saw_key = false, saw_city = false;
    bool open_net = false, clear_key = false, saw_nums = false;

    char *save = 0;
    for (char *pair = strtok_r(body, "&", &save); pair; pair = strtok_r(0, "&", &save)) {
        char *eq = strchr(pair, '=');
        if (!eq)
            continue;
        *eq = 0;
        url_decode(pair);
        url_decode(eq + 1);
        if (!strcmp(pair, "ssid")) {
            snprintf(ssid, sizeof(ssid), "%s", eq + 1);
            saw_ssid = true;
        } else if (!strcmp(pair, "pass")) {
            snprintf(pass, sizeof(pass), "%s", eq + 1);
            saw_pass = true;
        } else if (!strcmp(pair, "key")) {
            snprintf(key, sizeof(key), "%s", eq + 1);
            saw_key = true;
        } else if (!strcmp(pair, "city")) {
            snprintf(city, sizeof(city), "%s", eq + 1);
            saw_city = true;
        } else if (!strcmp(pair, "open") && !strcmp(eq + 1, "1")) {
            open_net = true;
        } else if (!strcmp(pair, "clear") && !strcmp(eq + 1, "1")) {
            clear_key = true;
        } else if (!strcmp(pair, "drink")) {
            snprintf(drink, sizeof(drink), "%s", eq + 1);
            saw_nums = true;
        } else if (!strcmp(pair, "move")) {
            snprintf(move, sizeof(move), "%s", eq + 1);
        } else if (!strcmp(pair, "q0")) {
            snprintf(q0, sizeof(q0), "%s", eq + 1);
        } else if (!strcmp(pair, "q1")) {
            snprintf(q1, sizeof(q1), "%s", eq + 1);
        } else if (!strcmp(pair, "tz")) {
            snprintf(tz, sizeof(tz), "%s", eq + 1);
        }
    }

    NetUpdate update = {0};
    if (saw_ssid) {
        snprintf(update.ssid, sizeof(update.ssid), "%s", ssid);
        update.has_ssid = true;
    }
    if (saw_pass) {
        snprintf(update.pass, sizeof(update.pass), "%s", pass);
        update.has_pass = true;
    }
    update.open_net = open_net;
    if (saw_key) {
        snprintf(update.key, sizeof(update.key), "%s", key);
        update.has_key = true;
    }
    update.clear_key = clear_key;
    if (saw_city) {
        snprintf(update.city, sizeof(update.city), "%s", city);
        update.has_city = true;
    }
    if (saw_nums) {
        update.has_nums = true;
        update.drink_min = clamp_int(drink, 45, 0, 240);
        update.move_min = clamp_int(move, 60, 0, 240);
        update.quiet_start = clamp_int(q0, 23, 0, 23);
        update.quiet_end = clamp_int(q1, 8, 0, 23);
        update.tz_hours = clamp_int(tz, 8, 8, 9);
        if (update.tz_hours != 8 && update.tz_hours != 9)
            update.tz_hours = 8;
    }
    net_apply_update(&update);

    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, "saved", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t redirect_root(httpd_req_t *req, httpd_err_code_t err)
{
    (void)err;
    httpd_resp_set_status(req, "302 Temporary Redirect");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, "Redirect", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static const httpd_uri_t root_uri = {.uri = "/", .method = HTTP_GET, .handler = root_get};
static const httpd_uri_t save_uri = {.uri = "/save", .method = HTTP_POST, .handler = save_post};

static void start_httpd(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_open_sockets = 4;
    config.lru_purge_enable = true;
    config.stack_size = 6144;
    httpd_handle_t server = 0;
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "http server failed");
        return;
    }
    httpd_register_uri_handler(server, &root_uri);
    httpd_register_uri_handler(server, &save_uri);
    httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, redirect_root);
    esp_log_level_set("httpd_uri", ESP_LOG_ERROR);
    esp_log_level_set("httpd_txrx", ESP_LOG_ERROR);
    esp_log_level_set("httpd_parse", ESP_LOG_ERROR);
}

static void captive_dhcp(void)
{
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (!netif)
        return;
    static const char uri[] = "http://192.168.4.1";
    esp_netif_dhcps_stop(netif);
    esp_err_t err = esp_netif_dhcps_option(netif, ESP_NETIF_OP_SET, ESP_NETIF_CAPTIVEPORTAL_URI,
                                           (void *)uri, sizeof(uri) - 1);
    if (err != ESP_OK)
        ESP_LOGW(TAG, "captive uri %s", esp_err_to_name(err));
    esp_netif_dhcps_start(netif);
}

static int dns_reply(char *req, int req_len, char *out, int out_len)
{
    if (req_len < 12 || req_len > out_len)
        return -1;
    memcpy(out, req, (size_t)req_len);
    uint8_t *header = (uint8_t *)out;
    header[2] |= 0x80;
    uint16_t questions = (uint16_t)((header[4] << 8) | header[5]);
    header[6] = 0;
    header[7] = 0;
    if (questions != 1)
        return req_len;

    int offset = 12;
    while (offset < req_len) {
        int label = (unsigned char)out[offset];
        if (label == 0) {
            ++offset;
            break;
        }
        if ((label & 0xc0) == 0xc0)
            return req_len;
        offset += label + 1;
    }
    if (offset + 4 > req_len)
        return req_len;
    uint16_t qtype = (uint16_t)(((unsigned char)out[offset] << 8) | (unsigned char)out[offset + 1]);
    uint16_t qclass = (uint16_t)(((unsigned char)out[offset + 2] << 8) | (unsigned char)out[offset + 3]);
    if (qtype != 1)
        return req_len;

    if (req_len + 16 > out_len)
        return req_len;
    uint8_t *answer = (uint8_t *)out + req_len;
    answer[0] = 0xc0;
    answer[1] = 0x0c;
    answer[2] = qtype >> 8;
    answer[3] = qtype & 0xff;
    answer[4] = qclass >> 8;
    answer[5] = qclass & 0xff;
    answer[6] = 0;
    answer[7] = 0;
    answer[8] = 0;
    answer[9] = 60;
    answer[10] = 0;
    answer[11] = 4;
    uint32_t ip = inet_addr("192.168.4.1");
    memcpy(answer + 12, &ip, 4);
    header[6] = 0;
    header[7] = 1;
    return req_len + 16;
}

static void dns_task(void *arg)
{
    (void)arg;
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "dns socket");
        vTaskDelete(0);
        return;
    }
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(53);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        ESP_LOGE(TAG, "dns bind");
        close(sock);
        vTaskDelete(0);
        return;
    }
    for (;;) {
        char req[256];
        char reply[320];
        struct sockaddr_in from;
        socklen_t from_len = sizeof(from);
        int len = recvfrom(sock, req, sizeof(req), 0, (struct sockaddr *)&from, &from_len);
        if (len <= 0)
            continue;
        int n = dns_reply(req, len, reply, (int)sizeof(reply));
        if (n > 0)
            sendto(sock, reply, (size_t)n, 0, (struct sockaddr *)&from, from_len);
    }
}

void portal_start(void)
{
    page_mu = xSemaphoreCreateMutex();
    captive_dhcp();
    start_httpd();
    xTaskCreate(dns_task, "dns", 3072, 0, 4, 0);
    ESP_LOGI(TAG, "portal %s / %s at http://192.168.4.1", PET_AP_SSID, PET_AP_PASS);
}
