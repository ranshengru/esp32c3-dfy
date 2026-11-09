#include "sonar.h"
#include "net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

// Wiring is unchanged from v0.4.
enum { PIN_ECHO = 6, PIN_TRIG = 7 };
enum {
    PERIOD_MS = 60,
    MIN_MM = 20,
    MAX_MM = 4000,
    FAR_MM = 4000,         // "nothing in front" baseline when echoes time out
    BASE_SAMPLES = 9,      // median window for the baseline
    BASE_READY = 5,
    FAR_AFTER_MISSES = 5,  // this many timeouts in a row: baseline is "far"
    CONFIRM = 2,           // consecutive qualifying readings to count a sweep
    RELEASE = 3,           // consecutive near-baseline readings to re-arm
    REFRACTORY_MS = 600,
    STUCK_MS = 5000,       // something parked in front: adopt it as the new baseline
};

static const char *TAG = "sonar";
static volatile int64_t s_rise_us;
static volatile int64_t s_fall_us;
static volatile int s_edges;
static int s_threshold_mm = 200;
static bool s_debug;
static unsigned s_base[BASE_SAMPLES];
static int s_base_len;
static int s_base_pos;
static int s_misses;
static unsigned s_last_mm;
static unsigned s_last_base;
static unsigned s_sweeps;

// Edge timestamps come from the GPIO interrupt, so Wi-Fi tasks preempting the
// sonar task no longer stretch the measured echo (the v0.4 busy-wait did).
static void IRAM_ATTR echo_isr(void *arg)
{
    (void)arg;
    int64_t now = esp_timer_get_time();
    if (gpio_get_level(PIN_ECHO)) {
        s_rise_us = now;
        s_edges = 1;
    } else if (s_edges == 1) {
        s_fall_us = now;
        s_edges = 2;
    }
}

static unsigned measure_mm(void)
{
    if (gpio_get_level(PIN_ECHO))
        return 0;
    s_edges = 0;
    gpio_set_level(PIN_TRIG, 1);
    esp_rom_delay_us(12);
    gpio_set_level(PIN_TRIG, 0);
    // Echo up to ~4 m is ~23 ms; wait 30 ms, then read the timestamps.
    vTaskDelay(pdMS_TO_TICKS(30));
    if (s_edges != 2)
        return 0;
    int64_t width = s_fall_us - s_rise_us;
    if (width <= 0)
        return 0;
    unsigned mm = (unsigned)((width * 10) / 58);
    return (mm < MIN_MM || mm > MAX_MM) ? 0 : mm;
}

static int cmp_uint(const void *a, const void *b)
{
    unsigned x = *(const unsigned *)a, y = *(const unsigned *)b;
    return x < y ? -1 : x > y;
}

static void base_reset(void)
{
    s_base_len = 0;
    s_base_pos = 0;
}

static void base_add(unsigned mm)
{
    s_base[s_base_pos] = mm;
    s_base_pos = (s_base_pos + 1) % BASE_SAMPLES;
    if (s_base_len < BASE_SAMPLES)
        ++s_base_len;
}

// 0 = unknown. FAR_MM when the sensor sees nothing.
static unsigned baseline(void)
{
    if (s_misses >= FAR_AFTER_MISSES)
        return FAR_MM;
    if (s_base_len < BASE_READY)
        return 0;
    unsigned copy[BASE_SAMPLES];
    memcpy(copy, s_base, sizeof(unsigned) * s_base_len);
    qsort(copy, s_base_len, sizeof(unsigned), cmp_uint);
    return copy[s_base_len / 2];
}

static void load_threshold(void)
{
    nvs_handle_t h;
    if (nvs_open("pet", NVS_READONLY, &h) != ESP_OK)
        return;
    char text[8] = {0};
    size_t len = sizeof(text);
    if (nvs_get_str(h, "sweep", text, &len) == ESP_OK) {
        int cm = atoi(text);
        if (cm >= 5 && cm <= 200)
            s_threshold_mm = cm * 10;
    }
    nvs_close(h);
}

int sonar_threshold_cm(void)
{
    return s_threshold_mm / 10;
}

bool sonar_set_threshold_cm(int cm)
{
    if (cm < 5 || cm > 200)
        return false;
    s_threshold_mm = cm * 10;
    nvs_handle_t h;
    if (nvs_open("pet", NVS_READWRITE, &h) == ESP_OK) {
        char text[8];
        snprintf(text, sizeof(text), "%d", cm);
        nvs_set_str(h, "sweep", text);
        nvs_commit(h);
        nvs_close(h);
    }
    return true;
}

void sonar_set_debug(bool on)
{
    s_debug = on;
}

void sonar_status(char *out, int size)
{
    snprintf(out, size, "sweep threshold %dcm last %umm baseline %umm sweeps %u", s_threshold_mm / 10,
             s_last_mm, s_last_base, s_sweeps);
}

static void sonar_task(void *arg)
{
    (void)arg;
    bool active = false;
    int confirm = 0;
    int release = 0;
    int64_t fired_us = 0;
    for (;;) {
        int64_t start = esp_timer_get_time();
        unsigned mm = measure_mm();
        int64_t now = esp_timer_get_time();
        if (mm) {
            s_misses = 0;
            s_last_mm = mm;
        } else if (s_misses < 1000) {
            ++s_misses;
        }
        unsigned base = baseline();
        s_last_base = base;
        if (s_debug)
            printf("sonar %u mm base %u %s\n", mm, base, active ? "active" : "idle");
        if (!active) {
            bool far_now = !mm;
            bool qualifies = mm && base && (mm > base ? mm - base : base - mm) > (unsigned)s_threshold_mm;
            if (qualifies) {
                if (++confirm >= CONFIRM) {
                    active = true;
                    confirm = 0;
                    release = 0;
                    fired_us = now;
                    ++s_sweeps;
                    printf("sweep %u: %u mm vs baseline %u mm (threshold %d cm)\n", s_sweeps, mm, base,
                           s_threshold_mm / 10);
                    fflush(stdout);
                    net_on_sweep();
                }
            } else {
                confirm = 0;
                if (mm)
                    base_add(mm);
                else if (far_now && s_misses >= FAR_AFTER_MISSES)
                    base_reset();
            }
        } else {
            bool near = base == FAR_MM ? (!mm || mm + (unsigned)s_threshold_mm / 2 >= FAR_MM)
                                       : (mm && (mm > base ? mm - base : base - mm) <= (unsigned)s_threshold_mm / 2);
            if (!mm && base != FAR_MM)
                near = false; // a lone timeout is not a return to baseline
            release = near ? release + 1 : 0;
            if (release >= RELEASE && now - fired_us >= REFRACTORY_MS * 1000LL) {
                active = false;
                release = 0;
            } else if (now - fired_us > STUCK_MS * 1000LL) {
                ESP_LOGI(TAG, "object stayed in front; re-learning baseline");
                active = false;
                release = 0;
                base_reset();
                s_misses = 0;
            }
        }
        int64_t spent_ms = (esp_timer_get_time() - start) / 1000;
        if (spent_ms < PERIOD_MS)
            vTaskDelay(pdMS_TO_TICKS(PERIOD_MS - spent_ms));
    }
}

void sonar_start(void)
{
    load_threshold();
    gpio_config_t trig = {.pin_bit_mask = 1ull << PIN_TRIG, .mode = GPIO_MODE_OUTPUT};
    gpio_config_t echo = {
        .pin_bit_mask = 1ull << PIN_ECHO,
        .mode = GPIO_MODE_INPUT,
        .intr_type = GPIO_INTR_ANYEDGE,
    };
    ESP_ERROR_CHECK(gpio_config(&trig));
    ESP_ERROR_CHECK(gpio_config(&echo));
    gpio_set_level(PIN_TRIG, 0);
    ESP_ERROR_CHECK(gpio_install_isr_service(0));
    ESP_ERROR_CHECK(gpio_isr_handler_add(PIN_ECHO, echo_isr, 0));
    xTaskCreate(sonar_task, "sonar", 3072, 0, 3, 0);
}
