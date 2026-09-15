#include "diagnostics.h"
#include "app_state.h"
#include "rtos.h"
#include <Arduino.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace {

struct DiagnosticEvent {
    uint32_t timestamp;
    char process[18];
    char message[142];
};

QueueHandle_t s_queue;
volatile bool s_enabled;
volatile uint32_t s_dropped;

void emit(const DiagnosticEvent &event) {
    Serial.printf("[DIAG %10lu] %-17s | %s\n",
                  (unsigned long)event.timestamp, event.process, event.message);
}

}  // namespace

void diagnosticsBegin(void) {
    if (!s_queue) s_queue = xQueueCreate(48, sizeof(DiagnosticEvent));
}

void diagnosticsSetEnabled(bool enabled) {
    s_enabled = enabled;
    Serial.printf("[diagnosticApp] %s\n",
                  enabled ? "ENABLED - live process and link tracing" : "DISABLED");
    if (enabled) {
        Serial.println("[diagnosticApp] use 'diagnosticApp off' to stop");
        Serial.println("[diagnosticApp] processes: mqttTask(network/publish), "
                       "otaTask(gateway OTA), hubTx/hubRx(Zigbee hub UART),");
        Serial.println("[diagnosticApp] sensor(node/reconcile), displayRx(HMI link), "
                       "relay(GPIO), mode(commands), linkTest, diagnostics");
    }
}

bool diagnosticsEnabled(void) {
    return s_enabled;
}

void diagnosticsLog(const char *process, const char *format, ...) {
    if (!s_enabled || !s_queue || !process || !format) return;
    DiagnosticEvent event{};
    event.timestamp = millis();
    strncpy(event.process, process, sizeof(event.process) - 1);
    va_list args;
    va_start(args, format);
    vsnprintf(event.message, sizeof(event.message), format, args);
    va_end(args);
    if (xQueueSend(s_queue, &event, 0) != pdTRUE) ++s_dropped;
}

void diagnosticsHex(const char *process, const char *direction,
                    const uint8_t *data, size_t length) {
    if (!s_enabled) return;
    char line[142];
    size_t used = (size_t)snprintf(line, sizeof(line), "%s %uB:",
                                   direction ? direction : "data",
                                   (unsigned)length);
    const size_t shown = length > 32 ? 32 : length;
    for (size_t i = 0; data && i < shown && used + 4 < sizeof(line); ++i) {
        used += (size_t)snprintf(line + used, sizeof(line) - used,
                                 " %02X", data[i]);
    }
    if (shown < length && used + 5 < sizeof(line)) {
        snprintf(line + used, sizeof(line) - used, " ...");
    }
    diagnosticsLog(process, "%s", line);
}

void diagnosticsTask(void *) {
    DiagnosticEvent event;
    uint32_t last_health = 0;
    for (;;) {
        if (s_queue && xQueueReceive(s_queue, &event, pdMS_TO_TICKS(100)) == pdTRUE) {
            emit(event);
        }
        const uint32_t now = millis();
        if (s_enabled && now - last_health >= 5000UL) {
            last_health = now;
            Serial.printf(
                "[DIAG %10lu] HEALTH            | mode=%u heap=%u "
                "queues display=%u hubTx=%u relay=%u mqttCmd=%u diagDrop=%lu\n",
                (unsigned long)now, operationMode, ESP.getFreeHeap(),
                displayCmdQueue ? (unsigned)uxQueueMessagesWaiting(displayCmdQueue) : 0,
                hubTxQueue ? (unsigned)uxQueueMessagesWaiting(hubTxQueue) : 0,
                relayQueue ? (unsigned)uxQueueMessagesWaiting(relayQueue) : 0,
                cmdQueue ? (unsigned)uxQueueMessagesWaiting(cmdQueue) : 0,
                (unsigned long)s_dropped);
        }
    }
}
