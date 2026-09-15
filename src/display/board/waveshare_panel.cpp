#include "waveshare_panel.h"
#include <Arduino.h>
#include <esp_display_panel.hpp>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <lvgl.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

using namespace esp_panel::board;
using namespace esp_panel::drivers;

static Board *s_board;
static SemaphoreHandle_t s_lvgl_mutex;
static lv_disp_draw_buf_t s_draw_buf;
static lv_color_t *s_buf_a;
static lv_color_t *s_buf_b;

namespace {

constexpr uint32_t kFrameBudgetUs = 16667;
constexpr uint32_t kProfilePeriodMs = 5000;
constexpr uint32_t kScreenPixels = 800u * 480u;

struct RenderMetrics {
    uint32_t frames;
    uint32_t flushes;
    uint64_t pixels;
    uint64_t flush_us;
    uint32_t max_flush_us;
    uint32_t handlers;
    uint64_t handler_us;
    uint32_t max_handler_us;
    uint32_t budget_misses;
};

RenderMetrics s_metrics;

void printAndResetMetrics(uint32_t elapsed_ms) {
    const uint32_t frames = s_metrics.frames;
    const uint32_t flushes = s_metrics.flushes;
    const uint32_t handlers = s_metrics.handlers;
    const uint32_t fps_x10 =
        elapsed_ms ? (uint32_t)((uint64_t)frames * 10000ULL / elapsed_ms) : 0;
    const uint32_t dirty_x10 =
        frames ? (uint32_t)(s_metrics.pixels * 1000ULL /
                            ((uint64_t)frames * kScreenPixels)) : 0;
    Serial.printf(
        "[HMI PERF] fps=%lu.%lu dirty=%lu.%lu%% "
        "handler_avg=%luus max=%luus misses=%lu "
        "flush_avg=%luus max=%luus flushes=%lu idle=%u%% heap=%u internal=%u\n",
        (unsigned long)(fps_x10 / 10), (unsigned long)(fps_x10 % 10),
        (unsigned long)(dirty_x10 / 10), (unsigned long)(dirty_x10 % 10),
        (unsigned long)(handlers ? s_metrics.handler_us / handlers : 0),
        (unsigned long)s_metrics.max_handler_us,
        (unsigned long)s_metrics.budget_misses,
        (unsigned long)(flushes ? s_metrics.flush_us / flushes : 0),
        (unsigned long)s_metrics.max_flush_us, (unsigned long)flushes,
        (unsigned)lv_timer_get_idle(), ESP.getFreeHeap(),
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    s_metrics = {};
}

}  // namespace

static void tickCallback(void *) {
    lv_tick_inc(2);
}

static void flushCallback(lv_disp_drv_t *driver, const lv_area_t *area, lv_color_t *pixels) {
    LCD *lcd = static_cast<LCD *>(driver->user_data);
    const int64_t started = esp_timer_get_time();
    lcd->drawBitmap(area->x1, area->y1, area->x2 - area->x1 + 1,
                    area->y2 - area->y1 + 1, reinterpret_cast<const uint8_t *>(pixels));
    const uint32_t duration = (uint32_t)(esp_timer_get_time() - started);
    ++s_metrics.flushes;
    s_metrics.flush_us += duration;
    if (duration > s_metrics.max_flush_us) s_metrics.max_flush_us = duration;
    s_metrics.pixels +=
        (uint64_t)(area->x2 - area->x1 + 1) * (area->y2 - area->y1 + 1);
    if (lv_disp_flush_is_last(driver)) ++s_metrics.frames;
    lv_disp_flush_ready(driver);
}

static void touchCallback(lv_indev_drv_t *driver, lv_indev_data_t *data) {
    Touch *touch = static_cast<Touch *>(driver->user_data);
    TouchPoint point;
    const int count = touch->readPoints(&point, 1, 0);
    if (count > 0) {
        data->state = LV_INDEV_STATE_PRESSED;
        data->point.x = point.x;
        data->point.y = point.y;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

static void lvglTask(void *) {
    TickType_t last_wake = xTaskGetTickCount();
    uint32_t last_profile = millis();
    for (;;) {
        if (waveshareLvglLock(50)) {
            const int64_t started = esp_timer_get_time();
            lv_timer_handler();
            const uint32_t duration = (uint32_t)(esp_timer_get_time() - started);
            ++s_metrics.handlers;
            s_metrics.handler_us += duration;
            if (duration > s_metrics.max_handler_us)
                s_metrics.max_handler_us = duration;
            if (duration > kFrameBudgetUs) ++s_metrics.budget_misses;

            const uint32_t now = millis();
            if (now - last_profile >= kProfilePeriodMs) {
                printAndResetMetrics(now - last_profile);
                last_profile = now;
            }
            waveshareLvglUnlock();
        }
        /* Service timers twice per 16 ms frame. The display refresh and
         * animation timers remain fixed at LV_DISP_DEF_REFR_PERIOD, while
         * this 8 ms scheduler cadence avoids uneven wake-up latency. */
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(8));
    }
}

bool wavesharePanelBegin() {
    s_board = new Board();
    if (!s_board || !s_board->init() || !s_board->begin()) {
        Serial.println("FATAL: Waveshare panel initialization failed");
        return false;
    }

    lv_init();
    // Six 80-line RGB565 flushes replace sixteen 30-line flushes per full frame.
    // Both buffers remain in PSRAM, so the larger transfer window does not consume
    // scarce internal SRAM.
    constexpr size_t pixels = 800u * 80u;
    s_buf_a = static_cast<lv_color_t *>(heap_caps_malloc(pixels * sizeof(lv_color_t),
                                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    s_buf_b = static_cast<lv_color_t *>(heap_caps_malloc(pixels * sizeof(lv_color_t),
                                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!s_buf_a || !s_buf_b) {
        Serial.println("FATAL: LVGL PSRAM buffers unavailable");
        return false;
    }
    lv_disp_draw_buf_init(&s_draw_buf, s_buf_a, s_buf_b, pixels);

    static lv_disp_drv_t display_driver;
    lv_disp_drv_init(&display_driver);
    display_driver.hor_res = 800;
    display_driver.ver_res = 480;
    display_driver.flush_cb = flushCallback;
    display_driver.draw_buf = &s_draw_buf;
    display_driver.user_data = s_board->getLCD();
    display_driver.full_refresh = 0;
    display_driver.direct_mode = 0;
    lv_disp_drv_register(&display_driver);

    static lv_indev_drv_t touch_driver;
    lv_indev_drv_init(&touch_driver);
    touch_driver.type = LV_INDEV_TYPE_POINTER;
    touch_driver.read_cb = touchCallback;
    touch_driver.user_data = s_board->getTouch();
    lv_indev_drv_register(&touch_driver);

    const esp_timer_create_args_t tick_args = {
        .callback = tickCallback,
        .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "lv_tick",
        .skip_unhandled_events = true,
    };
    esp_timer_handle_t tick_timer;
    if (esp_timer_create(&tick_args, &tick_timer) != ESP_OK ||
        esp_timer_start_periodic(tick_timer, 2000) != ESP_OK) {
        return false;
    }

    s_lvgl_mutex = xSemaphoreCreateRecursiveMutex();
    return s_lvgl_mutex &&
           xTaskCreatePinnedToCore(lvglTask, "lvgl", 8192, nullptr, 2, nullptr, 1) == pdPASS;
}

bool waveshareLvglLock(int timeout_ms) {
    if (!s_lvgl_mutex) return false;
    TickType_t timeout = timeout_ms < 0 ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    return xSemaphoreTakeRecursive(s_lvgl_mutex, timeout) == pdTRUE;
}

void waveshareLvglUnlock() {
    if (s_lvgl_mutex) {
        xSemaphoreGiveRecursive(s_lvgl_mutex);
    }
}
