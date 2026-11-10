#include "portal.h"

#pragma GCC diagnostic ignored "-Wformat-truncation"

#include "net.h"
#include "assets.h"
#include "sonar.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_http_client.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "settings";
static SemaphoreHandle_t page_mu;
static char page[1536];
static size_t page_len;
static httpd_req_t *page_req;
static bool page_failed;

static void flush_page(void)
{
    if (page_len && !page_failed) {
        if (httpd_resp_send_chunk(page_req, page, (ssize_t)page_len) != ESP_OK)
            page_failed = true;
    }
    page_len = 0;
}

// Streams the page in chunks so the HTML can grow without a large buffer.
static void append(const char *format, ...)
{
    char piece[640];
    va_list args;
    va_start(args, format);
    int written = vsnprintf(piece, sizeof(piece), format, args);
    va_end(args);
    if (written < 0)
        return;
    size_t len = (size_t)written < sizeof(piece) ? (size_t)written : sizeof(piece) - 1;
    if (len >= sizeof(piece) - 1)
        ESP_LOGW(TAG, "page piece truncated");
    if (page_len + len > sizeof(page))
        flush_page();
    memcpy(page + page_len, piece, len);
    page_len += len;
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

static void fmt_coord(char *out, size_t size, int e6, bool show)
{
    if (!show) {
        out[0] = 0;
        return;
    }
    int neg = e6 < 0;
    int v = neg ? -e6 : e6;
    snprintf(out, size, "%s%d.%06d", neg ? "-" : "", v / 1000000, v % 1000000);
}

static void toggle(const char *name, bool on, const char *title, const char *hint)
{
    append("<label class=\"row\"><span><b>%s</b>%s%s%s</span><input type=\"checkbox\" class=\"sw\" name=\"%s\" value=\"1\"%s></label>",
           title, hint ? "<small>" : "", hint ? hint : "", hint ? "</small>" : "", name, on ? " checked" : "");
}

static void build_page(const NetSnapshot *snap)
{
    char ssid[sizeof(snap->ssid) * 6];
    char city[sizeof(snap->city) * 6];
    char ip[sizeof(snap->ip) * 6];
    char wifi[sizeof(snap->wifi) * 6];
    char bname[sizeof(snap->bname) * 6];
    char msg[sizeof(snap->msg) * 6];
    char weather[sizeof(snap->weather) * 6];
    char lat[16], lon[16];
    escape_html(snap->ssid, ssid, sizeof(ssid));
    escape_html(snap->city, city, sizeof(city));
    escape_html(snap->ip, ip, sizeof(ip));
    escape_html(snap->wifi, wifi, sizeof(wifi));
    escape_html(snap->bname, bname, sizeof(bname));
    escape_html(snap->msg, msg, sizeof(msg));
    escape_html(snap->weather, weather, sizeof(weather));
    fmt_coord(lat, sizeof(lat), snap->lat_e6, snap->geo_fixed);
    fmt_coord(lon, sizeof(lon), snap->lon_e6, snap->geo_fixed);

    append("<!doctype html><html lang=\"zh-CN\"><meta charset=\"utf-8\">");
    append("<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\"><meta name=\"theme-color\" content=\"#1f2a6b\">");
    append("<title>桌宠设置</title><style>");
    append(":root{--navy:#1f2a6b;--blue:#4f6fd8;--soft:#e8eefc;--line:#e3e8f4;--ink:#1b2340;--mute:#6b7592}"
           "*{box-sizing:border-box}body{margin:0;background:#f3f5fb;color:var(--ink);"
           "font:15px/1.5 -apple-system,BlinkMacSystemFont,\"PingFang SC\",\"Noto Sans SC\",system-ui,sans-serif}"
           "main{max-width:560px;margin:0 auto;padding:16px 14px 96px}");
    append(".hero{background:linear-gradient(135deg,#1f2a6b,#3b55c4 70%%,#6d8cf0);color:#fff;border-radius:22px;padding:18px 18px 16px;"
           "box-shadow:0 10px 30px rgba(31,42,107,.25)}.hero h1{margin:0;font-size:21px;letter-spacing:.5px}"
           ".hero .v{font-size:12px;background:rgba(255,255,255,.18);padding:2px 8px;border-radius:99px;margin-left:6px;vertical-align:3px}");
    append(".big{display:flex;align-items:baseline;gap:10px;margin:12px 0 10px}.big b{font-size:34px;font-weight:700;letter-spacing:1px}"
           ".big span{opacity:.9}.chips{display:flex;flex-wrap:wrap;gap:6px}.chip{font-size:12px;background:rgba(255,255,255,.16);"
           "padding:4px 10px;border-radius:99px}.chip.ok:before{content:\"\";display:inline-block;width:7px;height:7px;border-radius:50%%;"
           "background:#7cf0a8;margin-right:6px}.chip.bad:before{content:\"\";display:inline-block;width:7px;height:7px;border-radius:50%%;background:#ffb36b;margin-right:6px}");
    append(".card{background:#fff;border-radius:18px;padding:4px 16px 14px;margin:14px 0;box-shadow:0 1px 2px rgba(27,35,64,.06),0 4px 14px rgba(27,35,64,.04)}"
           ".card h2{font-size:13px;font-weight:600;color:var(--mute);margin:14px 0 4px;letter-spacing:1px}"
           ".f{display:block;margin:12px 0 0}.f>span{display:block;font-size:13px;color:var(--mute);margin-bottom:5px}");
    append("input:not([type=checkbox]):not([type=radio]),select{width:100%%;padding:11px 12px;border:1px solid var(--line);border-radius:12px;"
           "font:inherit;color:inherit;background:#f8f9fd;outline:none;-webkit-appearance:none}"
           "input:focus,select:focus{border-color:var(--blue);background:#fff;box-shadow:0 0 0 3px rgba(79,111,216,.15)}"
           ".grid{display:grid;grid-template-columns:1fr 1fr;gap:10px}small{display:block;font-size:12px;color:var(--mute)}");
    append(".seg{display:flex;background:var(--soft);border-radius:14px;padding:4px;margin-top:8px}.seg label{flex:1}.seg input{display:none}"
           ".seg span{display:block;text-align:center;padding:9px 0;border-radius:11px;color:var(--mute);font-weight:600;transition:.2s}"
           ".seg input:checked+span{background:#fff;color:var(--navy);box-shadow:0 2px 6px rgba(31,42,107,.15)}");
    append(".row{display:flex;align-items:center;justify-content:space-between;gap:12px;padding:12px 0;border-top:1px solid var(--line);margin-top:12px}"
           ".row b{font-weight:500}.sw{-webkit-appearance:none;appearance:none;flex:none;width:46px;height:28px;border-radius:99px;background:#d5dbea;"
           "position:relative;transition:.2s;margin:0}.sw:before{content:\"\";position:absolute;top:3px;left:3px;width:22px;height:22px;border-radius:50%%;"
           "background:#fff;box-shadow:0 1px 3px rgba(0,0,0,.2);transition:.2s}.sw:checked{background:var(--blue)}.sw:checked:before{left:21px}");
    append(".note{font-size:12px;color:var(--mute);margin:10px 0 0}.stats{display:grid;grid-template-columns:repeat(3,1fr);gap:8px;margin-top:12px}"
           ".stats div{background:var(--soft);border-radius:12px;padding:8px 10px}.stats b{display:block;font-size:16px;color:var(--navy)}"
           ".stats span{font-size:11px;color:var(--mute)}");
    append(".bar{position:fixed;left:0;right:0;bottom:0;padding:12px 14px calc(12px + env(safe-area-inset-bottom));"
           "background:linear-gradient(transparent,#f3f5fb 35%%)}.bar button{display:block;width:100%%;max-width:532px;margin:0 auto;border:0;"
           "border-radius:14px;padding:14px;font-size:16px;font-weight:600;font-family:inherit;color:#fff;background:var(--navy);box-shadow:0 8px 20px rgba(31,42,107,.3)}</style>");

    // Status header.
    append("<main><section class=\"hero\"><h1>桌宠<span class=\"v\">v1.0</span></h1>");
    append("<div class=\"big\"><b>%s</b><span>%s%s%s</span></div>", snap->time_ok ? snap->clock : "--:--",
           weather[0] ? weather : "天气未加载", city[0] ? " · " : "", city);
    append("<div class=\"chips\"><span class=\"chip %s\">%s %s</span><span class=\"chip\">%s</span><span class=\"chip\">%s</span>%s</div></section>",
           snap->connected ? "ok" : "bad", snap->connected ? "已连接" : (wifi[0] ? wifi : "未连接"), ssid,
           ip[0] ? ip : "无 IP", snap->birthday_now ? "生日模式" : "陪伴模式",
           snap->ap_active ? "<span class=\"chip bad\">配网热点 192.168.4.1</span>" : "");

    append("<form method=\"post\" action=\"/save\"><input type=\"hidden\" name=\"v5\" value=\"1\">");

    // 模式 / 生日
    append("<section class=\"card\"><h2>模式 / 生日</h2><div class=\"seg\">"
           "<label><input type=\"radio\" name=\"mode\" value=\"0\"%s><span>陪伴</span></label>"
           "<label><input type=\"radio\" name=\"mode\" value=\"1\"%s><span>生日</span></label></div>",
           snap->mode == 0 ? " checked" : "", snap->mode == 1 ? " checked" : "");
    append("<label class=\"f\"><span>寿星名字（建议 4 字以内）</span><input name=\"bname\" maxlength=\"30\" value=\"%s\" placeholder=\"名字\"></label>", bname);
    append("<div class=\"grid\"><label class=\"f\"><span>生日 · 月</span><input name=\"bm\" type=\"number\" min=\"0\" max=\"12\" value=\"%d\"></label>", snap->bmonth);
    append("<label class=\"f\"><span>生日 · 日</span><input name=\"bd\" type=\"number\" min=\"0\" max=\"31\" value=\"%d\"></label></div>", snap->bday);
    toggle("bauto", snap->bauto, "生日当天自动切换", "当天自动进入生日派对，第二天回到陪伴模式");
    append("<label class=\"f\"><span>屏幕留言（留空清除）</span><input name=\"msg\" maxlength=\"13\" value=\"%s\" placeholder=\"显示在时间卡片下方\"></label></section>", msg);

    append("<section class=\"card\"><h2>形象</h2><div class=\"seg\">"
           "<label><input type=\"radio\" name=\"expr\" value=\"0\"%s><span>经典</span></label>"
           "<label><input type=\"radio\" name=\"expr\" value=\"1\"%s><span>表情</span></label></div>",
           snap->expr_on ? "" : " checked", snap->expr_on ? " checked" : "");
    append("<p class=\"note\">%s。默认经典。表情包缺失时仍显示内置立绘。</p></section>", snap->assets);

    // 天气位置
    append("<section class=\"card\"><h2>天气位置</h2>");
    append("<label class=\"f\"><span>显示名称</span><input name=\"city\" maxlength=\"31\" value=\"%s\" placeholder=\"新建区\"></label>", city);
    append("<div class=\"grid\"><label class=\"f\"><span>纬度</span><input name=\"lat\" inputmode=\"decimal\" value=\"%s\" placeholder=\"28.6924\"></label>", lat);
    append("<label class=\"f\"><span>经度</span><input name=\"lon\" inputmode=\"decimal\" value=\"%s\" placeholder=\"115.8153\"></label></div>", lon);
    append("<p class=\"note\">填了经纬度就按坐标查天气；留空则按名称搜索（同名地方可能搜错）。名称留空关闭天气。</p></section>");

    // 提醒
    append("<section class=\"card\"><h2>提醒</h2>");
    append("<div class=\"grid\"><label class=\"f\"><span>喝水间隔（分钟）</span><input name=\"drink\" type=\"number\" min=\"0\" max=\"240\" value=\"%d\"></label>", snap->drink_min);
    append("<label class=\"f\"><span>久坐间隔（分钟）</span><input name=\"move\" type=\"number\" min=\"0\" max=\"240\" value=\"%d\"></label></div>", snap->move_min);
    append("<div class=\"grid\"><label class=\"f\"><span>勿扰开始（时）</span><input name=\"q0\" type=\"number\" min=\"0\" max=\"23\" value=\"%d\"></label>", snap->quiet_start);
    append("<label class=\"f\"><span>勿扰结束（时）</span><input name=\"q1\" type=\"number\" min=\"0\" max=\"23\" value=\"%d\"></label></div>", snap->quiet_end);
    toggle("rpause", snap->rpause, "暂停喝水和久坐提醒", 0);
    append("<p class=\"note\">间隔填 0 关闭；勿扰可跨午夜，开始等于结束表示关闭。今日喝水 %d 次。</p></section>", snap->drinks_today);

    // Wi-Fi
    append("<section class=\"card\"><h2>Wi-Fi</h2>");
    append("<label class=\"f\"><span>网络名称（2.4G）</span><input name=\"ssid\" maxlength=\"31\" value=\"%s\" autocomplete=\"off\"></label>", ssid);
    append("<label class=\"f\"><span>密码（留空不修改）</span><input name=\"pass\" type=\"password\" maxlength=\"63\" autocomplete=\"new-password\" placeholder=\"••••••••\"></label>");
    toggle("open", false, "开放网络（无密码）", 0);
    append("</section>");

    // 高级
    append("<section class=\"card\"><h2>高级</h2>");
    append("<label class=\"f\"><span>时区</span><select name=\"tz\"><option value=\"8\"%s>北京时间 UTC+8</option><option value=\"9\"%s>东京时间 UTC+9</option></select></label>",
           snap->tz_hours == 8 ? " selected" : "", snap->tz_hours == 9 ? " selected" : "");
    append("<label class=\"f\"><span>挥手灵敏度：距离变化超过多少厘米算一次（默认 20）</span><input name=\"sweep\" type=\"number\" min=\"5\" max=\"200\" value=\"%d\"></label>", snap->sweep_cm);
    append("<label class=\"f\"><span>DeepSeek API 密钥（%s，留空不修改）</span><input name=\"key\" type=\"password\" maxlength=\"127\" autocomplete=\"new-password\"></label>",
           snap->key_set ? "已设置" : "未设置");
    toggle("clear", false, "清除已保存的 API 密钥", 0);
    append("<div class=\"stats\"><div><b>%s</b><span>余额 %s</span></div><div><b>%d/%d</b><span>今日 AI 调用</span></div>"
           "<div><b>%u KB</b><span>空闲内存</span></div></div>",
           snap->total[0] ? snap->total : "—", snap->currency, snap->llm_today, snap->llm_cap,
           snap->heap_free / 1024);
    append("<p class=\"note\">最低空闲 %u KB · 运行 %u 分钟 · AI 语录用 deepseek-flash，每天最多 %d 次、约 12 小时一次 · 密码和密钥不会回显 · 地址 http://pet.local/</p></section>",
           snap->heap_min / 1024, snap->uptime_s / 60, snap->llm_cap);
    append("<div class=\"bar\"><button type=\"submit\">保存设置</button></div></form></main></html>");
}

static esp_err_t root_get(httpd_req_t *req)
{
    NetSnapshot snap;
    net_snapshot(&snap);
    xSemaphoreTake(page_mu, portMAX_DELAY);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    page_req = req;
    page_len = 0;
    page_failed = false;
    build_page(&snap);
    flush_page();
    esp_err_t err = page_failed ? ESP_FAIL : httpd_resp_send_chunk(req, NULL, 0);
    page_req = 0;
    xSemaphoreGive(page_mu);
    return err;
}

static esp_err_t status_get(httpd_req_t *req)
{
    NetSnapshot snap;
    net_snapshot(&snap);
    char out[640];
    snprintf(out, sizeof(out),
             "{\"version\":\"v1.0\",\"connected\":%s,\"ip\":\"%s\",\"mode\":%d,\"expr\":%d,\"birthday_now\":%s,"
             "\"days_to_birthday\":%d,\"weather_ok\":%s,\"llm_today\":%d,\"llm_cap\":%d,"
             "\"ap\":%s,\"heap\":%u,\"heap_min\":%u,\"uptime\":%u}",
             snap.connected ? "true" : "false", snap.ip, snap.mode, snap.expr_on, snap.birthday_now ? "true" : "false",
             snap.days_to_birthday, snap.weather[0] ? "true" : "false", snap.llm_today, snap.llm_cap,
             snap.ap_active ? "true" : "false", snap.heap_free, snap.heap_min, snap.uptime_s);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, out, HTTPD_RESP_USE_STRLEN);
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

// Copies at most size-1 bytes without cutting a UTF-8 sequence in half.
static void copy_trim(char *dst, size_t size, const char *src)
{
    size_t len = strlen(src);
    if (len >= size) {
        len = size - 1;
        while (len && ((unsigned char)src[len] & 0xc0) == 0x80)
            --len;
    }
    memcpy(dst, src, len);
    dst[len] = 0;
}

// Empty means "not given"; a bad number is reported, not fatal for the form.
static bool num_field(const char *text, int min, int max, int *out, bool *bad)
{
    if (!text[0])
        return false;
    if (!parse_number(text, min, max, out)) {
        *bad = true;
        return false;
    }
    return true;
}

static esp_err_t save_post(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len > 2048)
        return bad_request(req);
    static char body[2049]; // httpd runs handlers on one task
    int got = 0;
    while (got < req->content_len) {
        int count = httpd_req_recv(req, body + got, (size_t)(req->content_len - got));
        if (count == HTTPD_SOCK_ERR_TIMEOUT)
            continue;
        if (count <= 0)
            return ESP_FAIL;
        got += count;
    }
    body[got] = 0;
    ESP_LOGI(TAG, "save: %d bytes", got);

    NetSnapshot now;
    net_snapshot(&now);
    NetUpdate update = {0};
    bool saw_city = false, bad_num = false, bad_pass = false;
    int drink = now.drink_min, move = now.move_min, q0 = now.quiet_start, q1 = now.quiet_end, tz = now.tz_hours;
    bool saw_nums = false;
    bool v5 = false, saw_mode = false, bauto = false, rpause = false, saw_msg = false, saw_bname = false, saw_expr = false;
    int mode = 0, bm = now.bmonth, bd = now.bday, sweep_cm = 0, expr = now.expr_on;
    char lat[20] = {0}, lon[20] = {0};
    static char bname[32];
    static char msg[40];
    bname[0] = msg[0] = 0;
    char *save = 0;
    for (char *pair = strtok_r(body, "&", &save); pair; pair = strtok_r(0, "&", &save)) {
        char *eq = strchr(pair, '=');
        if (!eq)
            continue;
        *eq++ = 0;
        if (!url_decode(pair) || !url_decode(eq))
            return bad_request(req);
        int value;
        if (!strcmp(pair, "ssid")) {
            copy_trim(update.ssid, sizeof(update.ssid), eq);
            update.has_ssid = update.ssid[0] != 0;
        } else if (!strcmp(pair, "pass")) {
            if (eq[0] && (strlen(eq) < 8 || strlen(eq) >= sizeof(update.pass))) {
                bad_pass = true;
            } else if (eq[0]) {
                copy_trim(update.pass, sizeof(update.pass), eq);
                update.has_pass = true;
            }
        } else if (!strcmp(pair, "key")) {
            copy_trim(update.key, sizeof(update.key), eq);
            update.has_key = update.key[0] != 0;
        } else if (!strcmp(pair, "city")) {
            copy_trim(update.city, sizeof(update.city), eq);
            saw_city = true;
        } else if (!strcmp(pair, "open") && !strcmp(eq, "1")) {
            update.open_net = true;
        } else if (!strcmp(pair, "clear") && !strcmp(eq, "1")) {
            update.clear_key = true;
        } else if (!strcmp(pair, "drink")) {
            if (num_field(eq, 0, 240, &value, &bad_num)) drink = value, saw_nums = true;
        } else if (!strcmp(pair, "move")) {
            if (num_field(eq, 0, 240, &value, &bad_num)) move = value, saw_nums = true;
        } else if (!strcmp(pair, "q0")) {
            if (num_field(eq, 0, 23, &value, &bad_num)) q0 = value, saw_nums = true;
        } else if (!strcmp(pair, "q1")) {
            if (num_field(eq, 0, 23, &value, &bad_num)) q1 = value, saw_nums = true;
        } else if (!strcmp(pair, "tz")) {
            if (num_field(eq, 8, 9, &value, &bad_num)) tz = value, saw_nums = true;
        } else if (!strcmp(pair, "v5")) {
            v5 = true;
        } else if (!strcmp(pair, "mode")) {
            saw_mode = num_field(eq, 0, 1, &mode, &bad_num);
        } else if (!strcmp(pair, "expr")) {
            saw_expr = num_field(eq, 0, 1, &expr, &bad_num);
        } else if (!strcmp(pair, "bname")) {
            copy_trim(bname, sizeof(bname), eq);
            saw_bname = true;
        } else if (!strcmp(pair, "bm")) {
            if (num_field(eq, 0, 12, &value, &bad_num)) bm = value;
            else if (!eq[0]) bm = 0;
        } else if (!strcmp(pair, "bd")) {
            if (num_field(eq, 0, 31, &value, &bad_num)) bd = value;
            else if (!eq[0]) bd = 0;
        } else if (!strcmp(pair, "bauto") && !strcmp(eq, "1")) {
            bauto = true;
        } else if (!strcmp(pair, "rpause") && !strcmp(eq, "1")) {
            rpause = true;
        } else if (!strcmp(pair, "msg")) {
            copy_trim(msg, sizeof(msg), eq);
            saw_msg = true;
        } else if (!strcmp(pair, "sweep")) {
            if (num_field(eq, 5, 200, &value, &bad_num)) sweep_cm = value;
        } else if (!strcmp(pair, "lat")) {
            copy_trim(lat, sizeof(lat), eq);
        } else if (!strcmp(pair, "lon")) {
            copy_trim(lon, sizeof(lon), eq);
        }
    }
    bool use_coords = lat[0] && lon[0];
    bool bad_coords = false;
    double lat_v = 0, lon_v = 0;
    if (use_coords) {
        char *end1 = 0, *end2 = 0;
        lat_v = strtod(lat, &end1);
        lon_v = strtod(lon, &end2);
        if (!end1 || *end1 || !end2 || *end2 || lat_v < -90 || lat_v > 90 || lon_v < -180 || lon_v > 180) {
            use_coords = false;
            bad_coords = true;
        }
    } else if (saw_city) {
        update.has_city = true;
        update.geo_auto = true;
    }
    if (saw_nums) {
        update.has_nums = true;
        update.drink_min = drink;
        update.move_min = move;
        update.quiet_start = q0;
        update.quiet_end = q1;
        update.tz_hours = tz;
    }

    net_apply_update(&update);
    if (use_coords)
        net_set_location((int)(lat_v * 1000000.0 + (lat_v >= 0 ? 0.5 : -0.5)),
                         (int)(lon_v * 1000000.0 + (lon_v >= 0 ? 0.5 : -0.5)), update.city[0] ? update.city : 0);
    if (v5) {
        if (saw_mode)
            net_set_mode(mode);
        if (saw_expr)
            net_set_expr(expr);
        if (saw_bname)
            net_set_birthday(bname, (bm && bd) ? bm : 0, (bm && bd) ? bd : 0, bauto);
        if (saw_msg)
            net_set_message(msg);
        net_set_pause(rpause);
        if (sweep_cm)
            sonar_set_threshold_cm(sweep_cm);
    }
    ESP_LOGI(TAG, "save applied%s%s%s", bad_num ? ", bad number ignored" : "",
             bad_pass ? ", short password ignored" : "", bad_coords ? ", bad coordinates ignored" : "");

    char result[1024];
    snprintf(result, sizeof(result),
             "<!doctype html><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
             "%s<title>已保存</title><body style=\"font:16px -apple-system,system-ui,sans-serif;background:#f3f5fb;color:#1b2340;"
             "display:flex;min-height:90vh;align-items:center;justify-content:center\"><div style=\"background:#fff;border-radius:18px;"
             "padding:24px 22px;max-width:340px;box-shadow:0 4px 14px rgba(27,35,64,.08)\"><h2 style=\"margin:0 0 8px\">%s</h2>"
             "<p style=\"color:#6b7592\">%s%s%s</p><p><a href=\"/\" style=\"color:#4f6fd8\">返回设置</a></p></div>",
             (bad_num || bad_pass || bad_coords) ? "" : "<meta http-equiv=\"refresh\" content=\"2;url=/\">",
             (bad_num || bad_pass || bad_coords) ? "部分已保存" : "已保存 ✓",
             bad_pass ? "Wi-Fi 密码少于 8 位，没有修改。" : "", bad_coords ? "经纬度格式不对，没有修改。" : "",
             bad_num ? "有数字超出范围，已忽略。" : "其他设置已生效，2 秒后返回。");
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, result, HTTPD_RESP_USE_STRLEN);
}

// ---- WEBTEST: end-to-end check of the settings form over the real server ----
// Posts the form to 127.0.0.1 with the headers a phone browser sends, checks
// the change landed, then restores the previous values.

static const char *const PHONE_HEADERS[][2] = {
    {"Accept", "text/html,application/xhtml+xml,application/xml;q=0.9,image/avif,image/webp,image/apng,*/*;q=0.8,application/signed-exchange;v=b3;q=0.7"},
    {"Accept-Language", "zh-CN,zh-Hans;q=0.9,en-US;q=0.8,en;q=0.7"},
    {"Accept-Encoding", "gzip, deflate"},
    {"Cache-Control", "max-age=0"},
    {"Origin", "http://pet.local"},
    {"Referer", "http://pet.local/"},
    {"Upgrade-Insecure-Requests", "1"},
    {"Content-Type", "application/x-www-form-urlencoded"},
};

static int webtest_post(const char *form, bool phone)
{
    esp_http_client_config_t config = {
        .url = "http://127.0.0.1/save",
        .method = HTTP_METHOD_POST,
        .timeout_ms = 5000,
        .buffer_size_tx = 1536,
        .user_agent = phone ? "Mozilla/5.0 (iPhone; CPU iPhone OS 18_1 like Mac OS X) AppleWebKit/605.1.15 "
                              "(KHTML, like Gecko) Version/18.1 Mobile/15E148 Safari/604.1"
                            : "webtest",
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client)
        return -1;
    if (phone)
        for (unsigned i = 0; i < sizeof(PHONE_HEADERS) / sizeof(PHONE_HEADERS[0]); ++i)
            esp_http_client_set_header(client, PHONE_HEADERS[i][0], PHONE_HEADERS[i][1]);
    else
        esp_http_client_set_header(client, "Content-Type", "application/x-www-form-urlencoded");
    esp_http_client_set_post_field(client, form, (int)strlen(form));
    esp_err_t err = esp_http_client_perform(client);
    int status = err == ESP_OK ? esp_http_client_get_status_code(client) : -1;
    esp_http_client_cleanup(client);
    return status;
}

static void url_encode(const char *src, char *dst, size_t size)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t used = 0;
    for (; *src && used + 4 < size; ++src) {
        unsigned char c = (unsigned char)*src;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_') {
            dst[used++] = (char)c;
        } else if (c == ' ') {
            dst[used++] = '+';
        } else {
            dst[used++] = '%';
            dst[used++] = hex[c >> 4];
            dst[used++] = hex[c & 15];
        }
    }
    dst[used] = 0;
}

static void webtest_task(void *arg)
{
    (void)arg;
    NetSnapshot before;
    net_snapshot(&before);
    // Phone-style POST touching only msg / drink / sweep (Chinese text and a
    // '+' space included); birthday, Wi-Fi and location are left alone.
    char form[512];
    char msg[160];
    url_encode("网页 测试OK", msg, sizeof(msg));
    snprintf(form, sizeof(form),
             "v5=1&mode=%d&msg=%s&drink=%d&move=%d&q0=%d&q1=%d&pass=&tz=%d&sweep=%d%s",
             before.mode, msg, before.drink_min == 45 ? 50 : 45, before.move_min, before.quiet_start,
             before.quiet_end, before.tz_hours, before.sweep_cm == 20 ? 23 : 20, before.rpause ? "&rpause=1" : "");
    int minimal = webtest_post("v5=1", false);
    int phone = webtest_post(form, true);
    NetSnapshot after;
    net_snapshot(&after);
    bool msg_ok = !strcmp(after.msg, "网页 测试OK");
    bool drink_ok = after.drink_min == (before.drink_min == 45 ? 50 : 45);
    bool sweep_ok = after.sweep_cm == (before.sweep_cm == 20 ? 23 : 20);
    printf("WEBTEST minimal-headers POST -> %d\n", minimal);
    printf("WEBTEST phone-headers POST -> %d, msg %s, drink %d->%d %s, sweep %d->%d %s\n", phone,
           msg_ok ? "applied" : "NOT applied", before.drink_min, after.drink_min, drink_ok ? "ok" : "FAIL",
           before.sweep_cm, after.sweep_cm, sweep_ok ? "ok" : "FAIL");
    // Restore.
    url_encode(before.msg, msg, sizeof(msg));
    snprintf(form, sizeof(form), "v5=1&mode=%d&msg=%s&drink=%d&move=%d&q0=%d&q1=%d&tz=%d&sweep=%d%s", before.mode, msg,
             before.drink_min, before.move_min, before.quiet_start, before.quiet_end, before.tz_hours, before.sweep_cm,
             before.rpause ? "&rpause=1" : "");
    int restore = webtest_post(form, true);
    net_snapshot(&after);
    printf("WEBTEST restore -> %d, msg '%s' drink %d sweep %d\n", restore, after.msg, after.drink_min, after.sweep_cm);
    printf("WEBTEST %s\n", phone == 200 && msg_ok && drink_ok && sweep_ok ? "PASS" : "FAIL");
    fflush(stdout);
    vTaskDelete(0);
}

void portal_selftest(void)
{
    xTaskCreate(webtest_task, "webtest", 6144, 0, 3, 0);
}

static const httpd_uri_t root_uri = {.uri = "/", .method = HTTP_GET, .handler = root_get};
static const httpd_uri_t save_uri = {.uri = "/save", .method = HTTP_POST, .handler = save_post};
static const httpd_uri_t status_uri = {.uri = "/status", .method = HTTP_GET, .handler = status_get};

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
    httpd_register_uri_handler(server, &status_uri);
    ESP_LOGI(TAG, "settings page on port 80 (home network or setup hotspot)");
}
