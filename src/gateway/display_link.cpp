#include "display_link.h"
#include "app_state.h"
#include "button.h"
#include "config.h"
#include "event_log.h"
#include "hub_link.h"
#include "link_test.h"
#include "mode_engine.h"
#include "mqtt_service.h"
#include "ota_service.h"
#include "persistence.h"
#include "relay_control.h"
#include "rtos.h"
#include "wifi_prov.h"
#include "diagnostics.h"
#include <WiFi.h>
#include <esp_wifi.h>
#include <cstring>

static display_parser_t s_parser;
static DisplayLinkStats s_stats;
static uint16_t s_tx_sequence = 1;
static SemaphoreHandle_t s_echo_sem;
static uint16_t s_echo_sequence;
static uint32_t s_echo_started;
static uint16_t s_last_command_sequence;
static uint8_t s_last_command_type;
static display_ack_status_t s_last_command_status = DISPLAY_ACK_OK;

static const char *displayTypeName(uint8_t type) {
    switch (type) {
        case DISPLAY_MSG_HELLO: return "HELLO";
        case DISPLAY_MSG_SNAPSHOT_REQUEST: return "SNAPSHOT_REQUEST";
        case DISPLAY_MSG_SNAPSHOT: return "SNAPSHOT";
        case DISPLAY_MSG_HEARTBEAT: return "HEARTBEAT";
        case DISPLAY_MSG_ACK: return "ACK";
        case DISPLAY_MSG_ECHO: return "ECHO";
        case DISPLAY_MSG_WIFI_CREDENTIALS: return "WIFI_CREDENTIALS";
        case DISPLAY_MSG_HMI_OTA_STATUS: return "HMI_OTA_STATUS";
        case DISPLAY_CMD_RELAY: return "RELAY";
        case DISPLAY_CMD_MODE: return "MODE";
        case DISPLAY_CMD_PAIRING: return "PAIRING";
        case DISPLAY_CMD_FORGET_NODE: return "FORGET_NODE";
        case DISPLAY_CMD_FORGET_ALL: return "FORGET_ALL";
        case DISPLAY_CMD_OTA: return "GATEWAY_OTA";
        case DISPLAY_CMD_EVENT_LOG: return "EVENT_LOG";
        case DISPLAY_CMD_WIFI_RESET: return "WIFI_RESET";
        case DISPLAY_CMD_LINK_TEST: return "LINK_TEST";
        case DISPLAY_CMD_HMI_OTA: return "HMI_OTA";
        case DISPLAY_CMD_SENSOR_SLEEP: return "SENSOR_SLEEP";
        default: return "OTHER";
    }
}

static bool sendFrame(uint8_t type, uint16_t sequence, const void *payload, uint16_t length) {
    /* Nearly 800 bytes, and modeTask and linkTestTask both send frames. The
     * UART mutex already serializes senders, so encode inside it and keep the
     * buffer in .bss instead of on each caller's stack. */
    static uint8_t frame[DISPLAY_PROTO_FRAME_MAX];
    if (!displayUartMutex ||
        xSemaphoreTake(displayUartMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        diagnosticsLog("displayTx", "FAIL mutex timeout type=%s seq=%u",
                       displayTypeName(type), sequence);
        return false;
    }
    const size_t size = display_encode_frame(type, 0, sequence, payload, length, frame, sizeof(frame));
    if (!size) {
        xSemaphoreGive(displayUartMutex);
        diagnosticsLog("displayTx", "FAIL encode type=%s seq=%u len=%u",
                       displayTypeName(type), sequence, length);
        return false;
    }
    DisplaySerial.write(frame, size);
    DisplaySerial.flush();
    xSemaphoreGive(displayUartMutex);
    ++s_stats.txFrames;
    s_stats.lastTxMs = millis();
    diagnosticsLog("displayTx", "sent type=%s seq=%u payload=%u frame=%u",
                   displayTypeName(type), sequence, length, (unsigned)size);
    if (type != DISPLAY_MSG_SNAPSHOT && type != DISPLAY_MSG_WIFI_CREDENTIALS) {
        diagnosticsHex("displayTx", "payload", (const uint8_t *)payload, length);
    } else if (type == DISPLAY_MSG_WIFI_CREDENTIALS) {
        diagnosticsLog("displayTx", "credential payload redacted");
    }
    return true;
}

static void sendAck(uint16_t request, uint8_t command, display_ack_status_t status) {
    const display_ack_t ack = {request, (uint8_t)status, command};
    sendFrame(DISPLAY_MSG_ACK, s_tx_sequence++, &ack, sizeof(ack));
}

static void onFrame(const display_frame_t *frame, void *) {
    ++s_stats.rxFrames;
    s_stats.lastRxMs = millis();
    s_stats.online = true;
    diagnosticsLog("displayRx", "received type=%s seq=%u len=%u",
                   displayTypeName(frame->type), frame->sequence, frame->length);
    diagnosticsHex("displayRx", "payload", frame->payload, frame->length);
    if (frame->type == DISPLAY_MSG_HMI_OTA_STATUS &&
        frame->length == sizeof(display_hmi_ota_status_t)) {
        display_hmi_ota_status_t status;
        memcpy(&status, frame->payload, sizeof(status));
        status.message[sizeof(status.message) - 1] = '\0';
        Serial.printf("[HMI OTA] stage=%u result=%u %s\n",
                      status.stage, status.result, status.message);
        diagnosticsLog("hmiOta", "stage=%u result=%u %s",
                       status.stage, status.result, status.message);
        return;
    }
    if (frame->type == DISPLAY_MSG_ECHO && frame->sequence == s_echo_sequence) {
        if (s_echo_sem) xSemaphoreGive(s_echo_sem);
        return;
    }
    DisplayCommandItem item = {};
    item.type = frame->type;
    item.sequence = frame->sequence;
    item.length = frame->length;
    item.received_ms = millis();
    if (frame->length) memcpy(item.payload, frame->payload, frame->length);
    if (!displayCmdQueue || xQueueSend(displayCmdQueue, &item, 0) != pdTRUE) {
        diagnosticsLog("displayRx", "FAIL command queue full type=%s seq=%u",
                       displayTypeName(frame->type), frame->sequence);
        sendAck(frame->sequence, frame->type, DISPLAY_ACK_BUSY);
    } else {
        diagnosticsLog("displayRx", "queued type=%s seq=%u depth=%u",
                       displayTypeName(frame->type), frame->sequence,
                       (unsigned)uxQueueMessagesWaiting(displayCmdQueue));
    }
}

void displayBegin() {
    display_parser_init(&s_parser);
    s_echo_sem = xSemaphoreCreateBinary();
    DisplaySerial.begin(DISPLAY_UART_BAUD, SERIAL_8N1, DISPLAY_UART_RX, DISPLAY_UART_TX);
}

void displayGetStats(DisplayLinkStats *stats) {
    if (stats) {
        *stats = s_stats;
        stats->online = s_stats.online && millis() - s_stats.lastRxMs < 8000UL;
    }
}

void displayRxTask(void *) {
    uint8_t rx[128];
    for (;;) {
        size_t count = 0;
        while (DisplaySerial.available() && count < sizeof(rx)) {
            rx[count++] = (uint8_t)DisplaySerial.read();
        }
        if (count) {
            const uint32_t oldCrc = s_parser.crc_errors;
            const uint32_t oldFraming = s_parser.framing_errors;
            display_parser_push(&s_parser, rx, count, onFrame, nullptr);
            s_stats.crcErrors = s_parser.crc_errors;
            s_stats.framingErrors = s_parser.framing_errors;
            if (s_parser.crc_errors != oldCrc ||
                s_parser.framing_errors != oldFraming) {
                diagnosticsLog("displayRx",
                               "FAIL parser bytes=%u crc=%lu framing=%lu",
                               (unsigned)count,
                               (unsigned long)s_parser.crc_errors,
                               (unsigned long)s_parser.framing_errors);
            }
        }
        if (s_stats.online && millis() - s_stats.lastRxMs > 8000UL) {
            s_stats.online = false;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static void fillSnapshot(display_snapshot_t &out) {
    memset(&out, 0, sizeof(out));
    out.uptime_ms = millis();
    WiFi.macAddress(out.mac);
    IPAddress ip = WiFi.localIP();
    for (uint8_t i = 0; i < 4; ++i) out.ip[i] = ip[i];
    strncpy(out.firmware, CURRENT_VERSION, sizeof(out.firmware) - 1);
    out.wifi_state = wifi_flag;
    out.wifi_rssi = WiFi.status() == WL_CONNECTED ? (int8_t)WiFi.RSSI() : -127;
    out.mqtt_connected = mqttIsConnected();
    out.ota_available = ota_available;
    out.operation_mode = operationMode;
    if (relayMutex && xSemaphoreTake(relayMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        for (uint8_t i = 0; i < 4; ++i) {
            if (relayStates[i]) out.relay_mask |= (1u << i);
        }
        xSemaphoreGive(relayMutex);
    }
    out.pairing_open = pairingMode;
    if (pairingMode) {
        const uint32_t elapsed = millis() - pairingModeStartTime;
        out.pairing_remaining_sec = elapsed < PAIRING_MODE_DURATION_MS
            ? (uint16_t)((PAIRING_MODE_DURATION_MS - elapsed) / 1000UL) : 0;
    }
    if (nodesMutex && xSemaphoreTake(nodesMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        out.node_count = nodeCount;
        for (uint8_t i = 0; i < nodeCount && i < DISPLAY_PROTO_MAX_NODES; ++i) {
            out.nodes[i].node_id = nodes[i].nodeId;
            memcpy(out.nodes[i].ieee, nodes[i].ieee, 8);
            out.nodes[i].short_addr = nodes[i].shortAddr;
            out.nodes[i].temperature_x100 = (int16_t)(nodes[i].lastTemperature * 100.0f);
            out.nodes[i].humidity_x100 = (uint16_t)(nodes[i].lastHumidity * 100.0f);
            out.nodes[i].lqi = nodes[i].lqi;
            out.nodes[i].age_ms = nodes[i].lastSeen ? millis() - nodes[i].lastSeen : UINT32_MAX;
            out.nodes[i].active = nodes[i].active && out.nodes[i].age_ms < 300000UL;
            out.nodes[i].battery_percent = nodes[i].batteryPercent;
        }
        xSemaphoreGive(nodesMutex);
    }
    out.queue_running = queueRunning;
    out.queue_paused = queuePaused;
    out.queue_loop = queueConfig.loop;
    out.queue_autostart = queueConfig.autoStart;
    out.queue_step_count = queueConfig.stepCount;
    out.queue_current_step = currentQueueStep;
    if (queueRunning && !queuePaused && currentQueueStep < queueConfig.stepCount) {
        uint32_t elapsed = (millis() - queueStepStartTime) / 1000UL;
        out.queue_remaining_sec = elapsed < queueConfig.steps[currentQueueStep].duration
            ? queueConfig.steps[currentQueueStep].duration - elapsed : 0;
    }
    for (uint8_t i = 0; i < MAX_QUEUE_STEPS; ++i) {
        out.queue_steps[i].relay_mask = queueConfig.steps[i].relayMask;
        out.queue_steps[i].duration_sec = queueConfig.steps[i].duration;
    }
    for (uint8_t i = 0; i < MAX_AUTO_RULES; ++i) {
        display_rule_t &dst = out.rules[i];
        const AutomationRule &src = autoRules[i];
        dst.enabled = src.enabled;
        dst.node_id = src.nodeId;
        dst.relay_id = src.relayId;
        dst.flags = (src.useTempControl ? 1 : 0) | (src.useHumControl ? 2 : 0) | (src.logicOR ? 4 : 0);
        dst.temp_min_x100 = (int16_t)(src.tempMin * 100.0f);
        dst.temp_max_x100 = (int16_t)(src.tempMax * 100.0f);
        dst.hum_min_x100 = (uint16_t)(src.humMin * 100.0f);
        dst.hum_max_x100 = (uint16_t)(src.humMax * 100.0f);
        dst.active = src.lastState;
        if (src.enabled) ++out.rule_count;
    }
    out.hybrid_logic_or = hybridLogic;
    out.automation_active = automationActive;
    out.queue_active = queueActive;
    HubLinkStats hub;
    hubGetStats(&hub);
    out.hub_pan_id = hub.lastPanId;
    out.hub_channel = hub.lastChannel;
    out.hub_node_count = hub.lastNodeCount;
    const uint32_t hubLastRx = hub.lastStatusMs > hub.lastAckMs ? hub.lastStatusMs : hub.lastAckMs;
    out.hub_online = hub.rxFrames && millis() - hubLastRx < 15000UL;
    out.event_log_pending = getEventLogCount();
    out.gateway_display_tx = s_stats.txFrames;
    out.gateway_display_rx = s_stats.rxFrames;
    out.gateway_display_crc_errors = s_stats.crcErrors;
    out.sensor_sleep_sec = sensorSleepSec;
}

void displaySendSnapshot() {
    display_snapshot_t snapshot;
    fillSnapshot(snapshot);
    sendFrame(DISPLAY_MSG_SNAPSHOT, s_tx_sequence++, &snapshot, sizeof(snapshot));
}

static display_ack_status_t processCommand(const DisplayCommandItem &item) {
    switch (item.type) {
        case DISPLAY_MSG_HELLO:
        case DISPLAY_MSG_SNAPSHOT_REQUEST:
            displaySendSnapshot();
            return DISPLAY_ACK_OK;
        case DISPLAY_MSG_ECHO:
            sendFrame(DISPLAY_MSG_ECHO, item.sequence, item.payload, item.length);
            return DISPLAY_ACK_OK;
        case DISPLAY_CMD_RELAY: {
            if (item.length != sizeof(display_relay_command_t)) return DISPLAY_ACK_BAD_LENGTH;
            const display_relay_command_t *cmd = (const display_relay_command_t *)item.payload;
            if (cmd->relay_index != 0xFF && cmd->relay_index >= 4) return DISPLAY_ACK_BAD_VALUE;
            if (cmd->relay_index != 0xFF && operationMode != MODE_MANUAL) return DISPLAY_ACK_WRONG_MODE;
            if (cmd->relay_index == 0xFF) requestAllRelays(cmd->state != 0, 0);
            else requestRelay(cmd->relay_index, cmd->state != 0, 0);
            if (operationMode == MODE_MANUAL) saveManualModeConfig();
            return DISPLAY_ACK_OK;
        }
        case DISPLAY_CMD_MODE:
            if (item.length != 1 || item.payload[0] > MODE_HYBRID) return DISPLAY_ACK_BAD_VALUE;
            transitionToMode(item.payload[0], false);
            saveModeConfig();
            return DISPLAY_ACK_OK;
        case DISPLAY_CMD_PAIRING:
            if (item.length != 1) return DISPLAY_ACK_BAD_LENGTH;
            if (item.payload[0]) enablePairingMode(); else disablePairingMode();
            return DISPLAY_ACK_OK;
        case DISPLAY_CMD_FORGET_NODE: {
            if (item.length != 1 || item.payload[0] < 1 || item.payload[0] > MAX_NODES) return DISPLAY_ACK_BAD_VALUE;
            if (!nodesMutex || xSemaphoreTake(nodesMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
                return DISPLAY_ACK_BUSY;
            }
            int8_t idx = findNodeById(item.payload[0]);
            if (idx < 0) {
                xSemaphoreGive(nodesMutex);
                return DISPLAY_ACK_NOT_FOUND;
            }
            uint8_t ieee[8];
            memcpy(ieee, nodes[idx].ieee, sizeof(ieee));
            const uint8_t nodeId = nodes[idx].nodeId;
            for (uint8_t i = (uint8_t)idx; i + 1 < nodeCount; ++i) {
                nodes[i] = nodes[i + 1];
            }
            if (nodeCount) {
                --nodeCount;
                memset(&nodes[nodeCount], 0, sizeof(nodes[nodeCount]));
            }
            if (nodeId >= 1 && nodeId <= MAX_NODES) {
                temps[nodeId - 1] = 0;
                hums[nodeId - 1] = 0;
            }
            xSemaphoreGive(nodesMutex);
            hubRequestForgetNode(nodeId, ieee);
            saveNodesToEEPROM();
            diagnosticsLog("mode/display", "forgot gateway node=%u; hub removal queued",
                           nodeId);
            return DISPLAY_ACK_OK;
        }
        case DISPLAY_CMD_FORGET_ALL:
            sendAck(item.sequence, item.type, DISPLAY_ACK_OK);
            delay(100);
            resetPairedNodes();
            return DISPLAY_ACK_OK;
        case DISPLAY_CMD_QUEUE_CONFIG: {
            if (item.length < 3) return DISPLAY_ACK_BAD_LENGTH;
            const display_queue_config_command_t *cmd = (const display_queue_config_command_t *)item.payload;
            if (cmd->step_count > MAX_QUEUE_STEPS ||
                item.length != 3 + cmd->step_count * sizeof(display_queue_step_t)) return DISPLAY_ACK_BAD_LENGTH;
            queueRunning = false;
            queueConfig.loop = cmd->loop;
            queueConfig.autoStart = cmd->autostart;
            queueConfig.stepCount = cmd->step_count;
            for (uint8_t i = 0; i < cmd->step_count; ++i) {
                if (cmd->steps[i].duration_sec > 86400UL) return DISPLAY_ACK_BAD_VALUE;
                queueConfig.steps[i].relayMask = cmd->steps[i].relay_mask & 0x0F;
                queueConfig.steps[i].duration = cmd->steps[i].duration_sec;
            }
            saveQueueConfig();
            return DISPLAY_ACK_OK;
        }
        case DISPLAY_CMD_QUEUE_CONTROL:
            if (item.length != 1 || item.payload[0] > DISPLAY_QUEUE_RESTART) return DISPLAY_ACK_BAD_VALUE;
            if (item.payload[0] == DISPLAY_QUEUE_PAUSE) queuePaused = true;
            else if (item.payload[0] == DISPLAY_QUEUE_RESUME) queuePaused = false;
            else if (item.payload[0] == DISPLAY_QUEUE_STOP) {
                queueRunning = false; queuePaused = false; requestAllRelays(false, 2);
            } else if (queueConfig.stepCount) {
                currentQueueStep = 0; queueStepStartTime = millis(); queueRunning = true; queuePaused = false;
            }
            return DISPLAY_ACK_OK;
        case DISPLAY_CMD_AUTOMATION_CONFIG: {
            if (item.length < 1) return DISPLAY_ACK_BAD_LENGTH;
            const display_automation_config_command_t *cmd =
                (const display_automation_config_command_t *)item.payload;
            if (cmd->rule_count > MAX_AUTO_RULES ||
                item.length != 1 + cmd->rule_count * sizeof(display_rule_t)) return DISPLAY_ACK_BAD_LENGTH;
            initializeAutomationRules();
            uint8_t destination = 0;
            for (uint8_t i = 0; i < cmd->rule_count; ++i) {
                const display_rule_t &src = cmd->rules[i];
                if (!src.enabled) continue;
                if (src.node_id < 1 || src.node_id > MAX_NODES ||
                    src.relay_id < 1 || src.relay_id > 4 ||
                    src.temp_min_x100 > src.temp_max_x100 ||
                    src.hum_min_x100 > src.hum_max_x100) return DISPLAY_ACK_BAD_VALUE;
                AutomationRule &dst = autoRules[destination++];
                dst.enabled = src.enabled;
                dst.nodeId = src.node_id;
                dst.relayId = src.relay_id;
                dst.useTempControl = src.flags & 1;
                dst.useHumControl = src.flags & 2;
                dst.logicOR = src.flags & 4;
                dst.tempMin = src.temp_min_x100 / 100.0f;
                dst.tempMax = src.temp_max_x100 / 100.0f;
                dst.humMin = src.hum_min_x100 / 100.0f;
                dst.humMax = src.hum_max_x100 / 100.0f;
            }
            saveAutomationConfig();
            return DISPLAY_ACK_OK;
        }
        case DISPLAY_CMD_HYBRID_CONFIG:
            if (item.length != 1) return DISPLAY_ACK_BAD_LENGTH;
            hybridLogic = item.payload[0] ? 1 : 0;
            saveHybridConfig();
            return DISPLAY_ACK_OK;
        case DISPLAY_CMD_OTA:
            if (item.length != 1) return DISPLAY_ACK_BAD_LENGTH;
            if (item.payload[0]) requestOtaUpdate(); else requestOtaCheck();
            return DISPLAY_ACK_OK;
        case DISPLAY_CMD_EVENT_LOG:
            if (item.length != 1) return DISPLAY_ACK_BAD_LENGTH;
            if (item.payload[0]) clearEventLogs(); else sendEventLogs();
            return DISPLAY_ACK_OK;
        case DISPLAY_CMD_WIFI_RESET:
            sendAck(item.sequence, item.type, DISPLAY_ACK_OK);
            delay(100);
            resetWiFiCredentials();
            return DISPLAY_ACK_OK;
        case DISPLAY_CMD_LINK_TEST:
            if (item.length != 1) return DISPLAY_ACK_BAD_LENGTH;
            linkTestRequest(item.payload[0] == 1 ? LINK_TEST_HUB : LINK_TEST_DISPLAY_TX);
            return DISPLAY_ACK_OK;
        case DISPLAY_CMD_HMI_OTA: {
            if (item.length != 1 || item.payload[0] > 1) {
                return DISPLAY_ACK_BAD_LENGTH;
            }
            if (WiFi.status() != WL_CONNECTED) return DISPLAY_ACK_BUSY;
            wifi_config_t wifiConfig{};
            if (esp_wifi_get_config(WIFI_IF_STA, &wifiConfig) != ESP_OK ||
                wifiConfig.sta.ssid[0] == '\0') {
                diagnosticsLog("mode/display",
                               "FAIL HMI OTA credentials unavailable");
                return DISPLAY_ACK_INTERNAL;
            }
            display_wifi_credentials_t credentials{};
            credentials.action = item.payload[0];
            strncpy(credentials.ssid, (const char *)wifiConfig.sta.ssid,
                    sizeof(credentials.ssid) - 1);
            strncpy(credentials.password, (const char *)wifiConfig.sta.password,
                    sizeof(credentials.password) - 1);
            const bool sent = sendFrame(DISPLAY_MSG_WIFI_CREDENTIALS,
                                        s_tx_sequence++, &credentials,
                                        sizeof(credentials));
            memset(&credentials, 0, sizeof(credentials));
            diagnosticsLog("mode/display", "HMI OTA credential transfer %s",
                           sent ? "sent" : "FAILED");
            return sent ? DISPLAY_ACK_OK : DISPLAY_ACK_INTERNAL;
        }
        case DISPLAY_CMD_SENSOR_SLEEP: {
            if (item.length != 2) return DISPLAY_ACK_BAD_LENGTH;
            const uint16_t seconds = (uint16_t)item.payload[0] |
                                     ((uint16_t)item.payload[1] << 8);
            applySensorSleep(seconds);
            return DISPLAY_ACK_OK;
        }
        default:
            return DISPLAY_ACK_BAD_VALUE;
    }
}

void displayProcessPendingCommands() {
    /* Carries a 768-byte payload and is drained only by modeTask. */
    static DisplayCommandItem item;
    while (displayCmdQueue && xQueueReceive(displayCmdQueue, &item, 0) == pdTRUE) {
        const bool acknowledgedCommand =
            item.type != DISPLAY_MSG_SNAPSHOT_REQUEST && item.type != DISPLAY_MSG_HELLO &&
            item.type != DISPLAY_MSG_ECHO && item.type != DISPLAY_CMD_WIFI_RESET;
        if (acknowledgedCommand && item.sequence == s_last_command_sequence &&
            item.type == s_last_command_type) {
            sendAck(item.sequence, item.type, s_last_command_status);
            continue;
        }
        const display_ack_status_t status = processCommand(item);
        diagnosticsLog("mode/display",
                       "processed type=%s seq=%u status=%u latency=%lums",
                       displayTypeName(item.type), item.sequence, status,
                       (unsigned long)(millis() - item.received_ms));
        if (acknowledgedCommand) {
            s_last_command_sequence = item.sequence;
            s_last_command_type = item.type;
            s_last_command_status = status;
            sendAck(item.sequence, item.type, status);
        }
        if (status == DISPLAY_ACK_OK && item.type != DISPLAY_MSG_SNAPSHOT_REQUEST &&
            item.type != DISPLAY_MSG_HELLO && item.type != DISPLAY_MSG_ECHO) {
            displaySendSnapshot();
            publishStatus();
        }
    }
}

bool displayEchoTest(uint32_t timeoutMs, uint32_t *rttMs) {
    if (!s_echo_sem) return false;
    xQueueReset(s_echo_sem);
    s_echo_sequence = s_tx_sequence++;
    s_echo_started = millis();
    const uint8_t bytes[4] = {0x53, 0x33, 0x48, 0x4D};
    sendFrame(DISPLAY_MSG_ECHO, s_echo_sequence, bytes, sizeof(bytes));
    if (xSemaphoreTake(s_echo_sem, pdMS_TO_TICKS(timeoutMs)) != pdTRUE) return false;
    if (rttMs) *rttMs = millis() - s_echo_started;
    return true;
}
