#include "ui.h"
#include "ui_controller.h"
#include "waveshare_panel.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

void uiBegin() {
    uiControllerBegin();
}

void uiRefreshTask(void *) {
    TickType_t last_wake = xTaskGetTickCount();
    for (;;) {
        if (waveshareLvglLock(8)) {
            uiControllerRefresh();
            waveshareLvglUnlock();
        }
        /* Fast tier: input feedback/ACKs at 60 Hz. uiControllerRefresh()
         * internally gates normal data and static widgets to slower rates. */
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(16));
    }
}
