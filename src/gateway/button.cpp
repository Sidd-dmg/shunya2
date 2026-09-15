#include "button.h"
#include "app_state.h"
#include "config.h"
#include "hub_link.h"
#include "display_link.h"
#include "wifi_prov.h"
#include "persistence.h"
#include "relay_control.h"
#include "rtos.h"
#include <WiFi.h>
#include <Arduino.h>
#include <cstring>

void enablePairingMode(void) {
    pair_mode = 1;
    pairingMode = true;
    pairingModeStartTime = millis();
    control_states[5] = 1;
    hubRequestPairOpen((uint16_t)(PAIRING_MODE_DURATION_MS / 1000UL));
    displaySendSnapshot();
    Serial.println("Pairing mode enabled (Zigbee permit-join requested)");
}

void disablePairingMode(void) {
    pairingMode = false;
    pair_mode = 0;
    control_states[5] = 0;
    hubRequestPairClose();
    displaySendSnapshot();
    Serial.println("Pairing mode disabled");
}

void resetPairedNodes(void) {
    Serial.println("\n=== Resetting paired Zigbee sensors ===");
    hubRequestForgetAll();
    if (xSemaphoreTake(nodesMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
        nodeCount = 0;
        memset(nodes, 0, sizeof(nodes));
        xSemaphoreGive(nodesMutex);
    }
    saveNodesToEEPROM();
    delay(500);
    ESP.restart();
}

void factoryReset(void) {
    Serial.println("FACTORY RESET");
    requestAllRelays(false, 0);
    delay(200);
    hubRequestForgetAll();
    delay(200);
    eepromFlushAll();
    WiFi.disconnect(true, true);
    delay(1000);
    ESP.restart();
}

void checkButtonPress(void) {
    bool currentButtonState = (digitalRead(BOOT_BUTTON_PIN) == LOW);
    if (currentButtonState && !buttonPressed) {
        buttonPressed = true;
        buttonPressStartTime = millis();
    } else if (!currentButtonState && buttonPressed) {
        unsigned long pressDuration = millis() - buttonPressStartTime;
        if (pressDuration >= FACTORY_RESET_TIME_MS) {
            factoryReset();
        } else if (pressDuration >= RESET_NODES_TIME_MS) {
            resetPairedNodes();
        } else if (pressDuration >= RESET_WIFI_TIME_MS) {
            resetWiFiCredentials();
        } else if (pressDuration >= BUTTON_HOLD_TIME_MS && !pairingMode) {
            enablePairingMode();
        }
        buttonPressed = false;
    }
}
