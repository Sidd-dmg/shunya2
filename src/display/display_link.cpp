#include "display_link.h"
#include "display_state.h"
#include "hmi_ota.h"
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <cstring>

static HardwareSerial s_gateway(1);
struct DisplayTxItem {
    uint8_t type;
    uint16_t sequence;
    uint16_t length;
    uint8_t payload[DISPLAY_PROTO_MAX_PAYLOAD];
};

static QueueHandle_t s_command_queue;
static QueueHandle_t s_service_queue;
static display_parser_t s_parser;
static uint16_t s_sequence = 1;
static uint32_t s_tx_dropped;
static bool s_last_valid;
static bool s_last_resent;
static DisplayTxItem s_last;
static uint32_t s_last_retry_request_ms;

static bool queueFrame(uint8_t type, uint16_t sequence, const void *payload,
                       uint16_t length, bool priority) {
    QueueHandle_t queue = priority ? s_command_queue : s_service_queue;
    if (!queue || length > DISPLAY_PROTO_MAX_PAYLOAD ||
        (length && !payload)) {
        ++s_tx_dropped;
        Serial.printf("[displayLink] TX reject type=0x%02X len=%u dropped=%lu\n",
                      type, length, (unsigned long)s_tx_dropped);
        return false;
    }
    DisplayTxItem item{};
    item.type = type;
    item.sequence = sequence;
    item.length = length;
    if (length) {
        memcpy(item.payload, payload, length);
    }
    const BaseType_t queued = xQueueSendToBack(queue, &item, 0);
    if (queued != pdTRUE) {
        ++s_tx_dropped;
        Serial.printf("[displayLink] TX queue full type=0x%02X seq=%u dropped=%lu\n",
                      type, sequence, (unsigned long)s_tx_dropped);
    }
    return queued == pdTRUE;
}

static void sendNow(const DisplayTxItem &item, bool remember = true) {
    static uint8_t buffer[DISPLAY_PROTO_FRAME_MAX];
    const size_t encoded = display_encode_frame(
        item.type, 0, item.sequence, item.payload, item.length,
        buffer, sizeof(buffer));
    if (!encoded) return;
    s_gateway.write(buffer, encoded);
    if (encoded <= 64) {
        s_gateway.flush();
    }
    if (remember && item.type != DISPLAY_MSG_LINK_RETRY &&
        item.type != DISPLAY_MSG_HEARTBEAT) {
        s_last = item;
        s_last_valid = true;
        s_last_resent = false;
    }
}

static void resendLastFrame() {
    if (!s_last_valid || s_last_resent || s_last.type == DISPLAY_MSG_LINK_RETRY) {
        return;
    }
    s_last_resent = true;
    sendNow(s_last, false);
}

static void requestRetransmit() {
    const uint32_t now = millis();
    if (now - s_last_retry_request_ms < 30UL) {
        return;
    }
    s_last_retry_request_ms = now;
    queueFrame(DISPLAY_MSG_LINK_RETRY, s_sequence++, nullptr, 0, true);
}

static void onFrame(const display_frame_t *frame, void *) {
    if (frame->type == DISPLAY_MSG_LINK_RETRY) {
        resendLastFrame();
    } else if (frame->type == DISPLAY_MSG_SNAPSHOT && frame->length == sizeof(display_snapshot_t)) {
        display_snapshot_t snapshot;
        memcpy(&snapshot, frame->payload, sizeof(snapshot));
        displayStateSetSnapshot(snapshot);
    } else if (frame->type == DISPLAY_MSG_ACK && frame->length == sizeof(display_ack_t)) {
        display_ack_t ack;
        memcpy(&ack, frame->payload, sizeof(ack));
        displayStateSetAck(ack);
    } else if (frame->type == DISPLAY_MSG_HEARTBEAT) {
        displayStateSetLink(true);
    } else if (frame->type == DISPLAY_MSG_ECHO) {
        queueFrame(DISPLAY_MSG_ECHO, frame->sequence, frame->payload,
                   frame->length, true);
    } else if (frame->type == DISPLAY_MSG_WIFI_CREDENTIALS &&
               frame->length == sizeof(display_wifi_credentials_t)) {
        display_wifi_credentials_t credentials;
        memcpy(&credentials, frame->payload, sizeof(credentials));
        credentials.ssid[sizeof(credentials.ssid) - 1] = '\0';
        credentials.password[sizeof(credentials.password) - 1] = '\0';
        if (!hmiOtaSubmit(credentials)) {
            display_hmi_ota_status_t status{};
            status.stage = 1;
            status.result = 2;
            strncpy(status.message, "HMI OTA service busy",
                    sizeof(status.message) - 1);
            displayStateSetOtaStatus(status);
        }
        memset(&credentials, 0, sizeof(credentials));
    } else if (frame->type == DISPLAY_MSG_WIFI_PROV &&
               frame->length == sizeof(display_wifi_prov_t)) {
        display_wifi_prov_t info;
        memcpy(&info, frame->payload, sizeof(info));
        displayStateSetWifiProv(info);
    }
}

void displayLinkBegin() {
    s_command_queue = xQueueCreate(8, sizeof(DisplayTxItem));
    s_service_queue = xQueueCreate(4, sizeof(DisplayTxItem));
    display_parser_init(&s_parser);
    // Waveshare UART terminal: GPIO43 TX, GPIO44 RX, 115200 8N1.
    // A gateway snapshot can be nearly 780 bytes. Keep enough headroom for it
    // if the LVGL or Wi-Fi task briefly preempts this reader.
    s_gateway.setRxBufferSize(2048);
    s_gateway.setTxBufferSize(2048);
    s_gateway.begin(115200, SERIAL_8N1, 44, 43);
    const uint8_t hello[2] = {DISPLAY_PROTO_VERSION, 0};
    queueFrame(DISPLAY_MSG_HELLO, s_sequence++, hello, sizeof(hello), false);
    displayRequestSnapshot();
}

bool displayLinkConnected() {
    return displayStateLinkConnected();
}

uint16_t displaySendCommand(uint8_t type, const void *payload, uint16_t length) {
    const uint16_t seq = s_sequence++;
    return queueFrame(type, seq, payload, length, true) ? seq : 0;
}

void displayRequestSnapshot() {
    queueFrame(DISPLAY_MSG_SNAPSHOT_REQUEST, s_sequence++, nullptr, 0, false);
}

void displayLinkTask(void *) {
    uint8_t rx[512];
    uint32_t last_hello = 0;
    uint32_t last_request = 0;
    uint32_t last_heartbeat = 0;
    for (;;) {
        DisplayTxItem tx;
        while (xQueueReceive(s_command_queue, &tx, 0) == pdTRUE) {
            sendNow(tx);
        }
        if (xQueueReceive(s_service_queue, &tx, 0) == pdTRUE) {
            sendNow(tx);
        }
        while (s_gateway.available()) {
            size_t count = 0;
            while (s_gateway.available() && count < sizeof(rx)) {
                rx[count++] = (uint8_t)s_gateway.read();
            }
            const uint32_t old_crc = s_parser.crc_errors;
            display_parser_push(&s_parser, rx, count, onFrame, nullptr);
            if (s_parser.crc_errors != old_crc) {
                requestRetransmit();
            }
        }
        const uint32_t now = millis();
        if (now - last_heartbeat >= DISPLAY_LINK_HEARTBEAT_MS) {
            queueFrame(DISPLAY_MSG_HEARTBEAT, s_sequence++, nullptr, 0, false);
            last_heartbeat = now;
        }
        if (!displayLinkConnected() && now - last_hello >= 2000UL) {
            const uint8_t hello[2] = {DISPLAY_PROTO_VERSION, 0};
            queueFrame(DISPLAY_MSG_HELLO, s_sequence++, hello, sizeof(hello), false);
            last_hello = now;
        }
        /* The gateway publishes a fresh snapshot every five seconds. Request
         * one only while recovering, instead of competing with that large
         * traffic on every three-second interval. */
        if (!displayLinkConnected() && now - last_request >= 2000UL) {
            displayRequestSnapshot();
            last_request = now;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
