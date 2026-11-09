#include "portal.h"

#include "net.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "settings";
static char page[4096];
static size_t page_len;
static SemaphoreHandle_t page_mu;

static void append(const char *format, ...)
{
    if (page_len >= sizeof(page) - 1)
        return;
    va_list args;
    va_start(args, format);
    int written = vsnprintf(page + page_len, sizeof(page) - page_len, format, args);
    va_end(args);
    if (written < 0)
        return;
    size_t remaining = sizeof(page) - page_len;
    page_len += (size_t)written < remaining ? (size_t)written : remaining - 1;
}

static void escape_html(const char *src, char *dst, size_t size)
{
    size_t used = 0;
    if (!size)
        return;
    for (; *src; ++src) {
        const char *replacement = 0;
        switch (*src) {
        case '&': replacement = "&amp;"; break;
        case '<': replacement = "&lt;"; break;
        case '>': replacement = "&gt;"; break;
        case '"': replacement = "&quot;"; break;
        case '\'': replacement = "&#39;"; break;
        default: break;
        }
        size_t count = replacement ? strlen(replacement) : 1;
        if (used + count >= size)
            break;
        if (replacement) {
            memcpy(dst + used, replacement, count);
            used += count;
        } else {
            dst[used++] = *src;
        }
    }
    dst[used] = 0;
}

static void build_page(const NetSnapshot *snap)
{
    char ssid[sizeof(snap->ssid) * 6];
    char city[sizeof(snap->city) * 6];
    char ip[sizeof(snap->ip) * 6];
    char wifi[sizeof(snap->wifi) * 6];
    escape_html(snap->ssid, ssid, sizeof(ssid));
    escape_html(snap->city, city, sizeof(city));
    escape_html(snap->ip, ip, sizeof(ip));
    escape_html(snap->wifi, wifi, sizeof(wifi));

    page_len = 0;
    page[0] = 0;
    append("<!doctype html><html lang=\"zh-CN\"><meta charset=\"utf-8\">");
    append("<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">");
    append("<title>小粤桌宠设置</title><style>");
    append("body{font:16px system-ui,sans-serif;max-width:620px;margin:24px auto;padding:0 16px;background:#f5f7fb;color:#19233b}");
    append("h1{font-size:24px}.card{background:white;border:1px solid #dce3f0;border-radius:16px;padding:18px;margin:14px 0}");
    append("label{display:block;margin:14px 0 6px}input,select{box-sizing:border-box;width:100%%;padding:10px;border:1px solid #bac7dd;border-radius:8px;font-size:16px}");
    append("input[type=checkbox]{width:auto}button{border:0;border-radius:10px;padding:12px 22px;background:#334b9b;color:white;font-size:16px}");
    append("small,.muted{color:#63708b}.grid{display:grid;grid-template-columns:1fr 1fr;gap:12px}</style>");
    append("<main><h1>小粤桌宠设置</h1><p class=\"muted\">设备通过家里的 Wi-Fi 联网；设置页只在同一局域网内可访问。</p>");
    append("<section class=\"card\"><b>连接状态</b><p>Wi-Fi：%s<br>网络：%s<br>设备地址：%s</p></section>",
            wifi[0] ? wifi : "未连接", ssid[0] ? ssid : "未设置", ip[0] ? ip : "等待分配");
    append("<form method=\"post\" action=\"/save\"><section class=\"card\"><h2>网络和 API</h2>");
    append("<label>家里 Wi-Fi 名称</label><input name=\"ssid\" maxlength=\"31\" value=\"%s\" autocomplete=\"off\">", ssid);
    append("<label>Wi-Fi 密码 <small>留空表示不修改</small></label><input name=\"pass\" type=\"password\" maxlength=\"63\" autocomplete=\"new-password\">");
    append("<label><input type=\"checkbox\" name=\"open\" value=\"1\"> Wi-Fi 是开放网络（无密码）</label>");
    append("<label>DeepSeek API 密钥 <small>留空表示不修改</small></label><input name=\"key\" type=\"password\" maxlength=\"127\" autocomplete=\"new-password\">");
    append("<label><input type=\"checkbox\" name=\"clear\" value=\"1\"> 清除已保存的 API 密钥</label></section>");
    append("<section class=\"card\"><h2>时间、天气和提醒</h2>");
    append("<label>城市 <small>留空可关闭天气</small></label><input name=\"city\" maxlength=\"31\" value=\"%s\">", city);
    append("<label>时区</label><select name=\"tz\"><option value=\"8\"%s>北京时间</option><option value=\"9\"%s>东京时间</option></select>",
            snap->tz_hours == 8 ? " selected" : "", snap->tz_hours == 9 ? " selected" : "");
    append("<div class=\"grid\"><div><label>喝水间隔（分钟，0 关闭）</label><input name=\"drink\" type=\"number\" min=\"0\" max=\"240\" value=\"%d\"></div>", snap->drink_min);
    append("<div><label>久坐间隔（分钟，0 关闭）</label><input name=\"move\" type=\"number\" min=\"0\" max=\"240\" value=\"%d\"></div></div>", snap->move_min);
    append("<div class=\"grid\"><div><label>勿扰开始（小时）</label><input name=\"q0\" type=\"number\" min=\"0\" max=\"23\" value=\"%d\"></div>", snap->quiet_start);
    append("<div><label>勿扰结束（小时）</label><input name=\"q1\" type=\"number\" min=\"0\" max=\"23\" value=\"%d\"></div></div>", snap->quiet_end);
    append("<p class=\"muted\">勿扰时段跨过午夜也适用；开始和结束相同表示关闭勿扰。</p>");
    append("<button type=\"submit\">保存设置</button></section></form>");
    append("<p class=\"muted\">密码和 API 密钥不会回显。若更换 Wi-Fi，电脑也要切换到新网络后重新打开设备地址。</p></main></html>");
}

static esp_err_t root_get(httpd_req_t *req)
{
    NetSnapshot snap;
    net_snapshot(&snap);
    xSemaphoreTake(page_mu, portMAX_DELAY);
    build_page(&snap);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_send(req, page, (ssize_t)page_len);
    xSemaphoreGive(page_mu);
    return err;
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool url_decode(char *text)
{
    char *read = text;
    char *write = text;
    while (*read) {
        if (*read == '+') {
            *write++ = ' ';
            ++read;
        } else if (*read == '%' && read[1] && read[2]) {
            int hi = hex_value(read[1]);
            int lo = hex_value(read[2]);
            if (hi < 0 || lo < 0)
                return false;
            *write++ = (char)((hi << 4) | lo);
            read += 3;
        } else {
            *write++ = *read++;
        }
    }
    *write = 0;
    return true;
}

static bool copy_value(char *dst, size_t size, const char *src)
{
    size_t len = strlen(src);
    if (len >= size)
        return false;
    memcpy(dst, src, len + 1);
    return true;
}

static bool parse_number(const char *text, int min, int max, int *out)
{
    char *end = 0;
    long value = strtol(text, &end, 10);
    if (!text[0] || !end || *end || value < min || value > max)
        return false;
    *out = (int)value;
    return true;
}

static esp_err_t bad_request(httpd_req_t *req)
{
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid settings");
    return ESP_FAIL;
}

static esp_err_t save_post(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len > 1024)
        return bad_request(req);
    char body[1025];
    int got = 0;
    while (got < req->content_len) {
        int count = httpd_req_recv(req, body + got, (size_t)(req->content_len - got));
        if (count <= 0)
            return ESP_FAIL;
        got += count;
    }
    body[got] = 0;

    NetUpdate update = {0};
    bool saw_ssid = false, saw_pass = false, saw_key = false, saw_city = false;
    bool saw_drink = false, saw_move = false, saw_q0 = false, saw_q1 = false, saw_tz = false;
    int drink = 0, move = 0, q0 = 23, q1 = 8, tz = 8;
    char *save = 0;
    for (char *pair = strtok_r(body, "&", &save); pair; pair = strtok_r(0, "&", &save)) {
        char *eq = strchr(pair, '=');
        if (!eq)
            continue;
        *eq++ = 0;
        if (!url_decode(pair) || !url_decode(eq))
            return bad_request(req);
        if (!strcmp(pair, "ssid")) {
            if (!copy_value(update.ssid, sizeof(update.ssid), eq)) return bad_request(req);
            saw_ssid = true;
        } else if (!strcmp(pair, "pass")) {
            if (!copy_value(update.pass, sizeof(update.pass), eq)) return bad_request(req);
            saw_pass = true;
        } else if (!strcmp(pair, "key")) {
            if (!copy_value(update.key, sizeof(update.key), eq)) return bad_request(req);
            saw_key = true;
        } else if (!strcmp(pair, "city")) {
            if (!copy_value(update.city, sizeof(update.city), eq)) return bad_request(req);
            saw_city = true;
        } else if (!strcmp(pair, "open") && !strcmp(eq, "1")) {
            update.open_net = true;
        } else if (!strcmp(pair, "clear") && !strcmp(eq, "1")) {
            update.clear_key = true;
        } else if (!strcmp(pair, "drink")) {
            saw_drink = parse_number(eq, 0, 240, &drink);
            if (!saw_drink) return bad_request(req);
        } else if (!strcmp(pair, "move")) {
            saw_move = parse_number(eq, 0, 240, &move);
            if (!saw_move) return bad_request(req);
        } else if (!strcmp(pair, "q0")) {
            saw_q0 = parse_number(eq, 0, 23, &q0);
            if (!saw_q0) return bad_request(req);
        } else if (!strcmp(pair, "q1")) {
            saw_q1 = parse_number(eq, 0, 23, &q1);
            if (!saw_q1) return bad_request(req);
        } else if (!strcmp(pair, "tz")) {
            saw_tz = parse_number(eq, 8, 9, &tz);
            if (!saw_tz) return bad_request(req);
        }
    }
    if (saw_ssid && update.ssid[0])
        update.has_ssid = true;
    if (saw_pass && update.pass[0]) {
        if (strlen(update.pass) < 8)
            return bad_request(req);
        update.has_pass = true;
    }
    if (saw_key && update.key[0])
        update.has_key = true;
    if (saw_city)
        update.has_city = true;
    if (saw_drink && saw_move && saw_q0 && saw_q1 && saw_tz) {
        update.has_nums = true;
        update.drink_min = drink;
        update.move_min = move;
        update.quiet_start = q0;
        update.quiet_end = q1;
        update.tz_hours = tz;
    }

    static const char result[] =
        "<!doctype html><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<title>已保存</title><h1>设置已保存</h1><p>设备正在应用新设置。返回桌宠设置页可继续修改。</p><p><a href=\"/\">返回设置</a></p>";
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t response = httpd_resp_send(req, result, HTTPD_RESP_USE_STRLEN);
    net_apply_update(&update);
    return response;
}

static const httpd_uri_t root_uri = {.uri = "/", .method = HTTP_GET, .handler = root_get};
static const httpd_uri_t save_uri = {.uri = "/save", .method = HTTP_POST, .handler = save_post};

void portal_start(void)
{
    page_mu = xSemaphoreCreateMutex();
    if (!page_mu) {
        ESP_LOGE(TAG, "settings page mutex failed");
        return;
    }
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_open_sockets = 3;
    config.lru_purge_enable = true;
    config.stack_size = 6144;
    httpd_handle_t server = 0;
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "settings server failed");
        return;
    }
    httpd_register_uri_handler(server, &root_uri);
    httpd_register_uri_handler(server, &save_uri);
    ESP_LOGI(TAG, "settings page is available at the station IP");
}
