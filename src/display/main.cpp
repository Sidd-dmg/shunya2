#include <Arduino.h>
#include "waveshare_panel.h"
#include "display_state.h"
#include "display_link.h"
#include "ui.h"
#include "display_protocol.h"
#include "display_config.h"
#include "hmi_ota.h"

void setup() {
    Serial.begin(115200);
    delay(250);
    Serial.println("\n=== Shunya Waveshare 7-inch HMI ===");
    Serial.printf("Firmware: %s  OTA client: %s\n", HMI_FIRMWARE_VERSION,
                  HMI_OTA_CLIENT_ID);
    Serial.printf("PSRAM: %u bytes\n", ESP.getPsramSize());
    if (!display_protocol_self_test()) {
        Serial.println("FATAL: display protocol self-test failed");
        for (;;) delay(1000);
    }

    displayStateBegin();
    if (!wavesharePanelBegin()) {
        Serial.println("Panel startup failed; stopping");
        for (;;) delay(1000);
    }
    if (waveshareLvglLock(-1)) {
        uiBegin();
        waveshareLvglUnlock();
    }
    displayLinkBegin();
    hmiOtaBegin();
    xTaskCreatePinnedToCore(displayLinkTask, "displayLink", 6144, nullptr, 4, nullptr, 0);
    xTaskCreatePinnedToCore(uiRefreshTask, "uiRefresh", 6144, nullptr, 1, nullptr, 0);
    Serial.println("HMI ready; gateway UART RX44 TX43");
}

void loop() {
    delay(1000);
}
