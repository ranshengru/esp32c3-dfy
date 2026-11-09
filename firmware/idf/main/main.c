#include "display.h"
#include "net.h"
#include "sweep.h"

#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

enum { PIN_ECHO = 6, PIN_TRIG = 7 };

static unsigned distance_mm(void)
{
    if (gpio_get_level(PIN_ECHO))
        return 0;
    gpio_set_level(PIN_TRIG, 1);
    esp_rom_delay_us(12);
    gpio_set_level(PIN_TRIG, 0);
    int64_t start = esp_timer_get_time();
    while (!gpio_get_level(PIN_ECHO)) {
        if (esp_timer_get_time() - start > 30000)
            return 0;
    }
    int64_t rise = esp_timer_get_time();
    while (gpio_get_level(PIN_ECHO)) {
        if (esp_timer_get_time() - rise > 30000)
            return 0;
    }
    int64_t elapsed = esp_timer_get_time() - rise;
    if (elapsed < 0)
        return 0;
    return (unsigned)((elapsed * 10) / 58);
}

static void sonar_init(void)
{
    gpio_config_t trig = {
        .pin_bit_mask = 1ull << PIN_TRIG,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config_t echo = {
        .pin_bit_mask = 1ull << PIN_ECHO,
        .mode = GPIO_MODE_INPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&trig));
    ESP_ERROR_CHECK(gpio_config(&echo));
    gpio_set_level(PIN_TRIG, 0);
}

static void sonar_task(void *arg)
{
    (void)arg;
    sonar_init();
    SweepGate gate = {0};
    for (;;) {
        unsigned mm = distance_mm();
        if (mm < 20 || mm > 4000)
            mm = 0;
        uint32_t tick = (uint32_t)(esp_timer_get_time() * 16);
        if (update_sweep(&gate, tick, mm)) {
            ESP_LOGI("sonar", "sweep");
            net_on_sweep();
        }
        vTaskDelay(pdMS_TO_TICKS(70));
    }
}

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

static void ui_task(void *arg)
{
    (void)arg;
    UiState previous;
    memset(&previous, 0xff, sizeof(previous));
    for (;;) {
        UiState current;
        net_compose(&current);
        if (memcmp(&current, &previous, sizeof(current))) {
            display_show(&current);
            previous = current;
        }
        vTaskDelay(pdMS_TO_TICKS(250));
    }
}

void app_main(void)
{
    setvbuf(stdin, NULL, _IONBF, 0);
    display_init();
    net_init();
    xTaskCreate(ui_task, "ui", 6144, 0, 4, 0);
    xTaskCreate(sonar_task, "sonar", 3072, 0, 3, 0);
    xTaskCreate(console_task, "console", 4096, 0, 3, 0);
}
