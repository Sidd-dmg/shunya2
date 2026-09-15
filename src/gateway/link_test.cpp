#include "link_test.h"
#include "hub_link.h"
#include "display_link.h"
#include "app_state.h"
#include "mqtt_service.h"
#include "config.h"
#include "diagnostics.h"
#include <Arduino.h>
#include <ArduinoJson.h>
#include <cstring>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

static QueueHandle_t linkTestQueue = NULL;
static uint8_t echoSeq = 1;

static void printHelp(void) {
    Serial.println();
    Serial.println("UART link test (USB serial, 115200). Type a command and Enter:");
    Serial.println("  help              this text");
    Serial.println("  stats             hub + display UART counters");
    Serial.println("  test hub          ping + echo + status to Zigbee hub (C6)");
    Serial.println("  test display      request/echo the Waveshare HMI");
    Serial.println("  test display-tx   push a full state snapshot");
    Serial.println("  test display-loop jumper gateway GPIO 43 TX to GPIO 44 RX");
    Serial.println("  test all          hub tests + display snapshot");
    Serial.println("  diagnosticApp     live process/action/UART diagnosis");
    Serial.println("  diagnosticApp off stop diagnosis");
    Serial.println("MQTT: {\"link_test\":\"hub\"|\"display\"|\"display-tx\"|\"display-loop\"|\"all\"|\"stats\"}");
    Serial.println();
}

static void publishResult(const char *name, bool pass, const char *detail) {
    JsonDocument doc;
    doc["link_test"] = name;
    doc["pass"] = pass;
    if (detail && detail[0]) {
        doc["detail"] = detail;
    }
    char buffer[384];
    serializeJson(doc, buffer);
    Serial.println(buffer);
    if (mqttIsConnected()) {
        mqttEnqueuePublish(mqtt_status_topic.c_str(), buffer, false);
    }
}

static void printStats(void) {
    HubLinkStats hub;
    DisplayLinkStats display;
    hubGetStats(&hub);
    displayGetStats(&display);
    Serial.println("--- hub UART (S3 TX5/RX4) ---");
    Serial.printf("  tx=%lu rx=%lu csum_err=%lu last_ack_cmd=0x%02X last_nack=0x%02X\n",
                  (unsigned long)hub.txFrames, (unsigned long)hub.rxFrames,
                  (unsigned long)hub.checksumErrors, hub.lastAckCmd, hub.lastNackCmd);
    Serial.printf("  pan=0x%04X ch=%u nodes=%u hub_uptime_ms=%lu\n",
                  hub.lastPanId, hub.lastChannel, hub.lastNodeCount,
                  (unsigned long)hub.lastUptimeMs);
    Serial.println("--- display UART (gateway GPIO RX4/TX5) ---");
    Serial.printf("  online=%u tx=%lu rx=%lu crc_err=%lu framing_err=%lu\n",
                  display.online, (unsigned long)display.txFrames, (unsigned long)display.rxFrames,
                  (unsigned long)display.crcErrors, (unsigned long)display.framingErrors);
}

static bool runHubTests(void) {
    uint32_t rtt = 0;
    Serial.println("HUB: ping...");
    bool pingOk = hubPingTest(500, &rtt);
    Serial.printf("HUB: ping %s rtt=%lu ms\n", pingOk ? "PASS" : "FAIL", (unsigned long)rtt);

    uint8_t payload[8];
    payload[0] = echoSeq++;
    payload[1] = 0xA5;
    payload[2] = 0x5A;
    payload[3] = 0xC3;
    payload[4] = 0x3C;
    payload[5] = 0x12;
    payload[6] = 0x34;
    payload[7] = 0x56;
    Serial.println("HUB: echo...");
    uint32_t echoRtt = 0;
    bool echoOk = hubEchoTest(payload, sizeof(payload), 500, &echoRtt);
    Serial.printf("HUB: echo %s rtt=%lu ms\n", echoOk ? "PASS" : "FAIL", (unsigned long)echoRtt);

    HubLinkStats st;
    Serial.println("HUB: status...");
    bool statusOk = hubStatusTest(800, &st);
    if (statusOk) {
        Serial.printf("HUB: status PASS pan=0x%04X ch=%u nodes=%u uptime=%lu\n",
                      st.lastPanId, st.lastChannel, st.lastNodeCount,
                      (unsigned long)st.lastUptimeMs);
    } else {
        Serial.println("HUB: status FAIL (no HUB_EVT_STATUS)");
    }

    bool pass = pingOk && statusOk;
    char detail[180];
    snprintf(detail, sizeof(detail), "ping=%s/%lums echo=%s/%lums status=%s pan=0x%04X",
             pingOk ? "ok" : "fail", (unsigned long)rtt,
             echoOk ? "ok" : "fail", (unsigned long)echoRtt,
             statusOk ? "ok" : "fail", statusOk ? st.lastPanId : 0);
    if (pingOk && statusOk && !echoOk) {
        Serial.println("HUB: echo missing — flash current thread_hub_c6 for HUB_CMD_ECHO");
    }
    publishResult("hub", pass, detail);
    return pass;
}

static bool runDisplayTx(void) {
    displaySendSnapshot();
    Serial.println("DISPLAY-TX: full snapshot sent");
    publishResult("display-tx", true, "snapshot sent; confirm on display");
    return true;
}

static bool runDisplayEcho(void) {
    uint32_t rtt = 0;
    const bool ok = displayEchoTest(1000, &rtt);
    char detail[80];
    snprintf(detail, sizeof(detail), "echo=%s rtt=%lums", ok ? "ok" : "timeout", (unsigned long)rtt);
    Serial.printf("DISPLAY: %s\n", detail);
    publishResult("display", ok, detail);
    return ok;
}

static bool runDisplayLoopback(void) {
    Serial.println("DISPLAY-LOOP: disconnect display and jumper gateway GPIO5 TX to GPIO4 RX.");
    return runDisplayEcho();
}

static void runAll(void) {
    bool hub = runHubTests();
    bool display = runDisplayTx();
    publishResult("all", hub && display, hub ? "hub ok" : "hub failed");
}

static void handleKind(uint8_t kind) {
    switch (kind) {
        case LINK_TEST_HUB:
            runHubTests();
            break;
        case LINK_TEST_DISPLAY_TX:
            runDisplayTx();
            break;
        case LINK_TEST_DISPLAY_ECHO:
            runDisplayEcho();
            break;
        case LINK_TEST_DISPLAY_LOOPBACK:
            runDisplayLoopback();
            break;
        case LINK_TEST_ALL:
            runAll();
            break;
        case LINK_TEST_STATS:
            printStats();
            publishResult("stats", true, "printed on serial");
            break;
        default:
            break;
    }
}

static void handleLine(String line) {
    line.trim();
    line.toLowerCase();
    if (line.length() == 0) {
        return;
    }
    if (line == "help" || line == "?") {
        printHelp();
    } else if (line == "stats") {
        handleKind(LINK_TEST_STATS);
    } else if (line == "test hub" || line == "hub") {
        handleKind(LINK_TEST_HUB);
    } else if (line == "test display-tx" || line == "display-tx") {
        handleKind(LINK_TEST_DISPLAY_TX);
    } else if (line == "test display" || line == "display") {
        handleKind(LINK_TEST_DISPLAY_ECHO);
    } else if (line == "test display-loop" || line == "display-loop") {
        handleKind(LINK_TEST_DISPLAY_LOOPBACK);
    } else if (line == "test all") {
        handleKind(LINK_TEST_ALL);
    } else if (line == "diagnosticapp") {
        diagnosticsSetEnabled(true);
    } else if (line == "diagnosticapp off") {
        diagnosticsSetEnabled(false);
    } else if (line == "diagnosticapp status") {
        Serial.printf("[diagnosticApp] %s\n",
                      diagnosticsEnabled() ? "ENABLED" : "DISABLED");
    } else {
        Serial.println("Unknown command. Type help");
    }
}

void linkTestBegin(void) {
    if (linkTestQueue == NULL) {
        linkTestQueue = xQueueCreate(4, sizeof(uint8_t));
    }
}

void linkTestRequest(uint8_t kind) {
    if (linkTestQueue) {
        xQueueSend(linkTestQueue, &kind, 0);
    }
}

void linkTestTask(void *pvParameters) {
    (void)pvParameters;
    String line;
    vTaskDelay(pdMS_TO_TICKS(500));
    printHelp();
    for (;;) {
        uint8_t kind = 0;
        if (linkTestQueue && xQueueReceive(linkTestQueue, &kind, 0) == pdTRUE) {
            handleKind(kind);
        }
        while (Serial.available()) {
            char c = (char)Serial.read();
            if (c == '\r') {
                continue;
            }
            if (c == '\n') {
                handleLine(line);
                line = "";
            } else if (line.length() < 80) {
                line += c;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
