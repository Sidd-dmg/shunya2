#include "hub_link.h"
#include "app_state.h"
#include "types.h"
#include "rtos.h"
#include "config.h"
#include "link_protocol.h"
#include "diagnostics.h"
#include <Arduino.h>
#include <HardwareSerial.h>
#include <cstring>

static HardwareSerial HubSerial(1);
static HubLinkStats hubStats = {};
static SemaphoreHandle_t hubReplyMutex = NULL;
static SemaphoreHandle_t hubReplySem = NULL;
static uint8_t hubWaitKind = 0;
static uint8_t hubWaitSeq = 0;
static uint32_t hubWaitStartMs = 0;
static uint32_t hubLastFrameMs = 0;

static const char *hubTypeName(uint8_t type) {
    switch (type) {
        case HUB_CMD_PAIR_OPEN: return "PAIR_OPEN";
        case HUB_CMD_PAIR_CLOSE: return "PAIR_CLOSE";
        case HUB_CMD_FORGET_NODE: return "FORGET_NODE";
        case HUB_CMD_FORGET_ALL: return "FORGET_ALL";
        case HUB_CMD_LIST_NODES: return "LIST_NODES";
        case HUB_CMD_GET_STATUS: return "GET_STATUS";
        case HUB_CMD_SET_SLEEP: return "SET_SLEEP";
        case HUB_CMD_PING: return "PING";
        case HUB_CMD_ECHO: return "ECHO";
        case HUB_EVT_SENSOR: return "SENSOR";
        case HUB_EVT_JOINED: return "JOINED";
        case HUB_EVT_LEFT: return "LEFT";
        case HUB_EVT_NODE_INFO: return "NODE_INFO";
        case HUB_EVT_STATUS: return "STATUS";
        case HUB_EVT_ACK: return "ACK";
        case HUB_EVT_NACK: return "NACK";
        default: return "OTHER";
    }
}

enum {
    HUB_WAIT_NONE = 0,
    HUB_WAIT_ACK_PING = 1,
    HUB_WAIT_STATUS = 2,
    HUB_WAIT_ECHO = 3
};

static void hubReplyInit(void) {
    if (hubReplyMutex == NULL) {
        hubReplyMutex = xSemaphoreCreateMutex();
    }
    if (hubReplySem == NULL) {
        hubReplySem = xSemaphoreCreateBinary();
    }
}

static void enqueueHubTx(uint8_t cmd, const uint8_t *payload, uint8_t length) {
    HubTxItem item;
    memset(&item, 0, sizeof(item));
    item.cmd = cmd;
    item.length = length;
    if (payload && length > 0) {
        memcpy(item.payload, payload, length > sizeof(item.payload) ? sizeof(item.payload) : length);
    }
    if (hubTxQueue) {
        if (xQueueSend(hubTxQueue, &item, pdMS_TO_TICKS(50)) != pdTRUE) {
            diagnosticsLog("hubTx", "FAIL queue full cmd=%s", hubTypeName(cmd));
        } else {
            diagnosticsLog("hubTx", "queued cmd=%s len=%u depth=%u",
                           hubTypeName(cmd), length,
                           (unsigned)uxQueueMessagesWaiting(hubTxQueue));
        }
    }
}

void hubBegin(void) {
    hubReplyInit();
    HubSerial.begin(HUB_UART_BAUD, SERIAL_8N1, HUB_UART_RX, HUB_UART_TX);
}

void hubRequestEcho(const uint8_t *payload, uint8_t length) {
    enqueueHubTx(HUB_CMD_ECHO, payload, length);
}

void hubGetStats(HubLinkStats *out) {
    if (out) {
        *out = hubStats;
    }
}

static bool hubArmWait(uint8_t kind, uint8_t seq) {
    hubReplyInit();
    if (hubReplyMutex == NULL || hubReplySem == NULL) {
        return false;
    }
    xSemaphoreTake(hubReplyMutex, portMAX_DELAY);
    hubWaitKind = kind;
    hubWaitSeq = seq;
    hubWaitStartMs = millis();
    xQueueReset(hubReplySem);
    xSemaphoreGive(hubReplyMutex);
    return true;
}

static void hubSignalWait(uint8_t kind, uint8_t seq) {
    if (hubReplyMutex == NULL || hubReplySem == NULL) {
        return;
    }
    if (xSemaphoreTake(hubReplyMutex, 0) != pdTRUE) {
        return;
    }
    if (hubWaitKind == kind && (kind != HUB_WAIT_ECHO || hubWaitSeq == seq)) {
        hubWaitKind = HUB_WAIT_NONE;
        xSemaphoreGive(hubReplySem);
    }
    xSemaphoreGive(hubReplyMutex);
}

bool hubPingTest(uint32_t timeoutMs, uint32_t *rttMs) {
    if (!hubArmWait(HUB_WAIT_ACK_PING, HUB_CMD_PING)) {
        return false;
    }
    hubRequestPing();
    if (xSemaphoreTake(hubReplySem, pdMS_TO_TICKS(timeoutMs)) != pdTRUE) {
        hubArmWait(HUB_WAIT_NONE, 0);
        return false;
    }
    if (rttMs) {
        *rttMs = millis() - hubWaitStartMs;
    }
    return true;
}

bool hubStatusTest(uint32_t timeoutMs, HubLinkStats *out) {
    if (!hubArmWait(HUB_WAIT_STATUS, 0)) {
        return false;
    }
    hubRequestStatus();
    if (xSemaphoreTake(hubReplySem, pdMS_TO_TICKS(timeoutMs)) != pdTRUE) {
        hubArmWait(HUB_WAIT_NONE, 0);
        return false;
    }
    if (out) {
        hubGetStats(out);
    }
    return true;
}

bool hubEchoTest(const uint8_t *payload, uint8_t length, uint32_t timeoutMs, uint32_t *rttMs) {
    uint8_t seq = (payload && length > 0) ? payload[0] : 0;
    if (!hubArmWait(HUB_WAIT_ECHO, seq)) {
        return false;
    }
    hubRequestEcho(payload, length);
    if (xSemaphoreTake(hubReplySem, pdMS_TO_TICKS(timeoutMs)) != pdTRUE) {
        hubArmWait(HUB_WAIT_NONE, 0);
        return false;
    }
    if (rttMs) {
        *rttMs = millis() - hubWaitStartMs;
    }
    return true;
}

void hubRequestPairOpen(uint16_t durationSec) {
    uint8_t p[2];
    p[0] = (uint8_t)(durationSec & 0xFF);
    p[1] = (uint8_t)(durationSec >> 8);
    enqueueHubTx(HUB_CMD_PAIR_OPEN, p, 2);
}

void hubRequestPairClose(void) {
    enqueueHubTx(HUB_CMD_PAIR_CLOSE, NULL, 0);
}

void hubRequestForgetNode(uint8_t nodeId, const uint8_t *ieee) {
    uint8_t p[9];
    p[0] = nodeId;
    if (ieee) {
        memcpy(&p[1], ieee, 8);
    } else {
        memset(&p[1], 0, 8);
    }
    enqueueHubTx(HUB_CMD_FORGET_NODE, p, 9);
}

void hubRequestForgetAll(void) {
    enqueueHubTx(HUB_CMD_FORGET_ALL, NULL, 0);
}

void hubRequestListNodes(void) {
    enqueueHubTx(HUB_CMD_LIST_NODES, NULL, 0);
}

void hubRequestStatus(void) {
    enqueueHubTx(HUB_CMD_GET_STATUS, NULL, 0);
}

void hubRequestPing(void) {
    enqueueHubTx(HUB_CMD_PING, NULL, 0);
}

void hubRequestSetSleep(uint16_t seconds) {
    uint8_t p[2];
    p[0] = (uint8_t)(seconds & 0xFF);
    p[1] = (uint8_t)(seconds >> 8);
    enqueueHubTx(HUB_CMD_SET_SLEEP, p, 2);
}

static void pushSensorItem(SensorEvtType type, const uint8_t *payload, uint8_t length) {
    SensorQueueItem item;
    memset(&item, 0, sizeof(item));
    item.type = type;
    item.batteryPercent = BATTERY_PCT_UNKNOWN;
    if (length < 1) {
        return;
    }
    item.nodeId = payload[0];
    if (length >= 9) {
        memcpy(item.ieee, &payload[1], 8);
    }
    if (length >= 11) {
        item.shortAddr = (uint16_t)payload[9] | ((uint16_t)payload[10] << 8);
    }
    if (type == SENSOR_EVT_DATA && length >= 20) {
        memcpy(&item.temperature, &payload[11], 4);
        memcpy(&item.humidity, &payload[15], 4);
        item.lqi = payload[19];
        if (length >= 21) {
            item.batteryPercent = payload[20];
        }
        if (length >= 23) {
            sensorSleepSec = shunya_clamp_sleep_sec(
                (uint16_t)payload[21] | ((uint16_t)payload[22] << 8));
        }
    } else if (length >= 12) {
        item.lqi = payload[11];
    }
    xQueueSend(sensorQueue, &item, pdMS_TO_TICKS(20));
}

static void handleHubFrame(uint8_t cmd, const uint8_t *payload, uint8_t length) {
    diagnosticsLog("hubRx", "received type=%s len=%u", hubTypeName(cmd), length);
    diagnosticsHex("hubRx", "payload", payload, length);
    switch (cmd) {
        case HUB_EVT_SENSOR:
            pushSensorItem(SENSOR_EVT_DATA, payload, length);
            break;
        case HUB_EVT_JOINED:
            pushSensorItem(SENSOR_EVT_JOINED, payload, length);
            break;
        case HUB_EVT_LEFT:
            pushSensorItem(SENSOR_EVT_LEFT, payload, length);
            break;
        case HUB_EVT_NODE_INFO:
            pushSensorItem(SENSOR_EVT_LIST, payload, length);
            break;
        case HUB_EVT_PAIR_STATE:
            if (length >= 3) {
                bool open = payload[0] != 0;
                if (!open && pairingMode) {
                    pairingMode = false;
                    pair_mode = 0;
                    control_states[5] = 0;
                }
            }
            break;
        case HUB_EVT_STATUS:
            if (length >= 8) {
                hubStats.lastPanId = (uint16_t)payload[0] | ((uint16_t)payload[1] << 8);
                hubStats.lastChannel = payload[2];
                hubStats.lastNodeCount = payload[3];
                memcpy(&hubStats.lastUptimeMs, &payload[4], 4);
                hubStats.lastStatusMs = millis();
            }
            hubSignalWait(HUB_WAIT_STATUS, 0);
            break;
        case HUB_EVT_ACK:
            hubStats.lastAckMs = millis();
            if (length >= 1) {
                hubStats.lastAckCmd = payload[0];
                if (payload[0] == HUB_CMD_PING) {
                    hubSignalWait(HUB_WAIT_ACK_PING, HUB_CMD_PING);
                }
            }
            break;
        case HUB_EVT_NACK:
            if (length >= 1) {
                hubStats.lastNackCmd = payload[0];
            }
            break;
        case HUB_EVT_ECHO:
            hubStats.lastEchoMs = millis();
            hubSignalWait(HUB_WAIT_ECHO, (length >= 1) ? payload[0] : 0);
            break;
        default:
            break;
    }
}

void hubTxTask(void *pvParameters) {
    (void)pvParameters;
    HubTxItem item;
    for (;;) {
        if (xQueueReceive(hubTxQueue, &item, portMAX_DELAY) == pdTRUE) {
            uint8_t frame[LINK_MAX_FRAME];
            uint8_t n = link_build_frame(item.cmd, item.payload, item.length, frame, sizeof(frame));
            if (n == 0) {
                diagnosticsLog("hubTx", "FAIL encode cmd=%s len=%u",
                               hubTypeName(item.cmd), item.length);
                continue;
            }
            if (xSemaphoreTake(hubUartMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                HubSerial.write(frame, n);
                hubStats.txFrames++;
                xSemaphoreGive(hubUartMutex);
                diagnosticsLog("hubTx", "sent cmd=%s payload=%u frame=%u",
                               hubTypeName(item.cmd), item.length, n);
                diagnosticsHex("hubTx", "frame", frame, n);
            } else {
                diagnosticsLog("hubTx", "FAIL UART mutex cmd=%s",
                               hubTypeName(item.cmd));
            }
        }
    }
}

void hubRxTask(void *pvParameters) {
    (void)pvParameters;
    static uint8_t rxBuffer[96];
    static int rxCount = 0;
    unsigned long lastList = 0;
    unsigned long lastPing = 0;
    hubRequestStatus();
    hubRequestListNodes();
    for (;;) {
        unsigned long now = millis();
        if (now - lastPing >= 10000UL) {
            hubRequestPing();
            lastPing = now;
        }
        if (now - lastList >= 60000UL) {
            hubRequestListNodes();
            lastList = now;
        }
        while (HubSerial.available() && rxCount < (int)sizeof(rxBuffer)) {
            rxBuffer[rxCount++] = (uint8_t)HubSerial.read();
        }
        int i = 0;
        while (i < rxCount) {
            if (rxBuffer[i] != LINK_START_BYTE) {
                i++;
                continue;
            }
            if (i + 3 > rxCount) {
                break;
            }
            uint8_t cmd = rxBuffer[i + 1];
            uint8_t length = rxBuffer[i + 2];
            int packetSize = 3 + length + 2;
            if (i + packetSize > rxCount) {
                break;
            }
            if (rxBuffer[i + packetSize - 1] != LINK_END_BYTE) {
                i++;
                continue;
            }
            if (rxBuffer[i + packetSize - 2] != link_checksum(&rxBuffer[i + 1], (uint8_t)(length + 2))) {
                hubStats.checksumErrors++;
                diagnosticsLog("hubRx", "FAIL checksum cmd=0x%02X len=%u",
                               cmd, length);
                i++;
                continue;
            }
            const bool reconnected =
                hubLastFrameMs == 0 || now - hubLastFrameMs > 15000UL;
            hubLastFrameMs = now;
            hubStats.rxFrames++;
            handleHubFrame(cmd, &rxBuffer[i + 3], length);
            if (reconnected) {
                diagnosticsLog("hubRx", "hub connected/reconnected; reconciling node list");
                hubRequestListNodes();
                hubRequestStatus();
            }
            i += packetSize;
        }
        if (i > 0) {
            memmove(rxBuffer, &rxBuffer[i], rxCount - i);
            rxCount -= i;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
