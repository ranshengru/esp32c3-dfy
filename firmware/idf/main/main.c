#include "display.h"
#include "assets.h"
#include "net.h"
#include "sonar.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static void console_task(void *arg)
{
    (void)arg;
    char line[160];
    int length = 0;
    for (;;) {
        int byte = getchar();
        if (byte == EOF) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (byte == '\r' || byte == '\n') {
            if (length) {
                line[length] = 0;
                net_handle_line(line);
                length = 0;
            }
            continue;
        }
        if (length < (int)sizeof(line) - 1)
            line[length++] = (char)byte;
    }
}

static int g_face_expr = -1;
static int g_face_frame;
static int64_t g_face_us;

static bool apply_face(const UiState *ui)
{
    if (!ui->expr_on) {
        display_set_portrait(0);
        g_face_expr = -1;
        return false;
    }
    int frames = assets_frames(ui->expr);
    int64_t now = esp_timer_get_time();
    int before = g_face_frame;
    if (frames < 1) {
        display_set_portrait(0);
        return false;
    }
    if ((int)ui->expr != g_face_expr) {
        g_face_expr = ui->expr;
        g_face_frame = 0;
        g_face_us = now;
    } else if (frames > 1 && now - g_face_us >= 450000) {
        g_face_frame = (g_face_frame + 1) % frames;
        g_face_us = now;
    }
    display_set_portrait(assets_frame(ui->expr, g_face_frame));
    return g_face_frame != before;
}

static void ui_task(void *arg)
{
    (void)arg;
    UiState previous;
    memset(&previous, 0xff, sizeof(previous));
    uint32_t played = 0, fx_played = 0, blow_played = 0;
    bool first = true;
    int64_t last_party_draw = 0;
    for (;;) {
        UiState current;
        net_compose(&current);
        bool face_tick = apply_face(&current);
        if (first) {
            played = current.anim_seq;
            blow_played = current.blow_seq;
            first = false;
        }
        int ms = display_fx_frame_ms();
        if (current.party && current.blow_seq != blow_played) {
            // Birthday sweep: candles flicker and go out, then fireworks + wish.
            blow_played = current.blow_seq;
            played = current.anim_seq;
            fx_played = current.fx_seq;
            int64_t start = esp_timer_get_time();
            int frames = display_blow_frames() + display_fireworks_frames();
            for (int frame = 0; frame < frames; ++frame) {
                int64_t wait = start + (int64_t)frame * ms * 1000 - esp_timer_get_time();
                if (wait > 1000)
                    vTaskDelay(pdMS_TO_TICKS(wait / 1000));
                if (frame == display_blow_frames())
                    net_compose(&current); // wish bubble is ready by now
                if (frame < display_blow_frames())
                    display_party_frame(&current, PARTY_BLOW, frame, (uint32_t)frame);
                else
                    display_party_frame(&current, PARTY_FIREWORKS, frame - display_blow_frames(), (uint32_t)frame);
            }
            ESP_LOGI("ui", "blow + fireworks %d frames %lld ms", frames,
                     (long long)((esp_timer_get_time() - start) / 1000));
            previous = current;
            continue;
        }
        if (current.party && current.fx_seq != fx_played) {
            fx_played = current.fx_seq;
            played = current.anim_seq;
            int64_t start = esp_timer_get_time();
            int frames = display_fireworks_frames();
            for (int frame = 0; frame < frames; ++frame) {
                int64_t wait = start + (int64_t)frame * ms * 1000 - esp_timer_get_time();
                if (wait > 1000)
                    vTaskDelay(pdMS_TO_TICKS(wait / 1000));
                display_party_frame(&current, PARTY_SHOW, frame, (uint32_t)frame);
            }
            ESP_LOGI("ui", "fireworks %d frames %lld ms", frames, (long long)((esp_timer_get_time() - start) / 1000));
            previous = current;
            continue;
        }
        if (current.party)
            played = current.anim_seq; // birthday sweeps use the candle animation
        if (current.anim_seq != played) {
            // v0.1 reaction: two quick squash-and-rebounds under a still bubble.
            played = current.anim_seq;
            int64_t start = esp_timer_get_time();
            int frames = display_anim_frames();
            for (int frame = 0; frame < frames; ++frame) {
                int64_t due = start + (int64_t)frame * 33333;
                int64_t wait = due - esp_timer_get_time();
                if (wait > 1000)
                    vTaskDelay(pdMS_TO_TICKS(wait / 1000));
                display_anim_frame(&current, frame);
            }
            ESP_LOGI("ui", "motion done %d frames %lld ms", frames,
                     (long long)((esp_timer_get_time() - start) / 1000));
            previous = current;
            continue;
        }
        if (current.party && esp_timer_get_time() - last_party_draw > 150000) {
            // Candle flicker on the birthday screen.
            display_show(&current);
            last_party_draw = esp_timer_get_time();
            previous = current;
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        if (memcmp(&current, &previous, sizeof(current)) || face_tick) {
            display_show(&current);
            previous = current;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void app_main(void)
{
    setvbuf(stdin, NULL, _IONBF, 0);
    display_init();
    assets_init();
    net_init();
    xTaskCreate(ui_task, "ui", 6144, 0, 4, 0);
    sonar_start();
    xTaskCreate(console_task, "console", 4096, 0, 3, 0);
}
