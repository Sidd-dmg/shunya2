#ifndef SHUNYA_DISPLAY_PROTOCOL_H
#define SHUNYA_DISPLAY_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DISPLAY_PROTO_MAGIC_0 0xA5u
#define DISPLAY_PROTO_MAGIC_1 0x5Au
#define DISPLAY_PROTO_VERSION 3u
#define DISPLAY_PROTO_MAX_PAYLOAD 768u
#define DISPLAY_PROTO_HEADER_SIZE 10u
#define DISPLAY_PROTO_FRAME_MAX (DISPLAY_PROTO_HEADER_SIZE + DISPLAY_PROTO_MAX_PAYLOAD + 2u)
#define DISPLAY_PROTO_MAX_NODES 16u

typedef enum {
    DISPLAY_MSG_HELLO = 0x01,
    DISPLAY_MSG_SNAPSHOT_REQUEST = 0x02,
    DISPLAY_MSG_SNAPSHOT = 0x03,
    DISPLAY_MSG_HEARTBEAT = 0x04,
    DISPLAY_MSG_ACK = 0x05,
    DISPLAY_MSG_ECHO = 0x06,
    DISPLAY_MSG_WIFI_CREDENTIALS = 0x07,
    DISPLAY_MSG_HMI_OTA_STATUS = 0x08,

    DISPLAY_CMD_RELAY = 0x20,
    DISPLAY_CMD_MODE = 0x21,
    DISPLAY_CMD_PAIRING = 0x22,
    DISPLAY_CMD_FORGET_NODE = 0x23,
    DISPLAY_CMD_FORGET_ALL = 0x24,
    DISPLAY_CMD_QUEUE_CONFIG = 0x25,
    DISPLAY_CMD_QUEUE_CONTROL = 0x26,
    DISPLAY_CMD_AUTOMATION_CONFIG = 0x27,
    DISPLAY_CMD_HYBRID_CONFIG = 0x28,
    DISPLAY_CMD_OTA = 0x29,
    DISPLAY_CMD_EVENT_LOG = 0x2A,
    DISPLAY_CMD_WIFI_RESET = 0x2B,
    DISPLAY_CMD_LINK_TEST = 0x2C,
    DISPLAY_CMD_HMI_OTA = 0x2D,
    DISPLAY_CMD_SENSOR_SLEEP = 0x2E
} display_message_type_t;

typedef enum {
    DISPLAY_ACK_OK = 0,
    DISPLAY_ACK_BAD_LENGTH = 1,
    DISPLAY_ACK_BAD_VALUE = 2,
    DISPLAY_ACK_WRONG_MODE = 3,
    DISPLAY_ACK_BUSY = 4,
    DISPLAY_ACK_NOT_FOUND = 5,
    DISPLAY_ACK_INTERNAL = 6
} display_ack_status_t;

typedef enum {
    DISPLAY_QUEUE_PAUSE = 0,
    DISPLAY_QUEUE_RESUME = 1,
    DISPLAY_QUEUE_STOP = 2,
    DISPLAY_QUEUE_RESTART = 3
} display_queue_action_t;

typedef struct __attribute__((packed)) {
    uint8_t node_id;
    uint8_t ieee[8];
    uint16_t short_addr;
    int16_t temperature_x100;
    uint16_t humidity_x100;
    uint8_t lqi;
    uint32_t age_ms;
    uint8_t active;
    uint8_t battery_percent;
} display_node_t;

typedef struct __attribute__((packed)) {
    uint8_t relay_mask;
    uint32_t duration_sec;
} display_queue_step_t;

typedef struct __attribute__((packed)) {
    uint8_t enabled;
    uint8_t node_id;
    uint8_t relay_id;
    uint8_t flags; /* bit0=temp, bit1=humidity, bit2=OR */
    int16_t temp_min_x100;
    int16_t temp_max_x100;
    uint16_t hum_min_x100;
    uint16_t hum_max_x100;
    uint8_t active;
} display_rule_t;

typedef struct __attribute__((packed)) {
    uint32_t uptime_ms;
    uint8_t mac[6];
    uint8_t ip[4];
    char firmware[16];
    uint8_t wifi_state;
    int8_t wifi_rssi;
    uint8_t mqtt_connected;
    uint8_t ota_available;
    uint8_t operation_mode;
    uint8_t relay_mask;
    uint8_t pairing_open;
    uint16_t pairing_remaining_sec;
    uint8_t node_count;
    display_node_t nodes[DISPLAY_PROTO_MAX_NODES];
    uint8_t queue_running;
    uint8_t queue_paused;
    uint8_t queue_loop;
    uint8_t queue_autostart;
    uint8_t queue_step_count;
    uint8_t queue_current_step;
    uint32_t queue_remaining_sec;
    display_queue_step_t queue_steps[20];
    uint8_t rule_count;
    display_rule_t rules[16];
    uint8_t hybrid_logic_or;
    uint8_t automation_active;
    uint8_t queue_active;
    uint16_t hub_pan_id;
    uint8_t hub_channel;
    uint8_t hub_node_count;
    uint8_t hub_online;
    uint32_t event_log_pending;
    uint32_t gateway_display_tx;
    uint32_t gateway_display_rx;
    uint32_t gateway_display_crc_errors;
    uint16_t sensor_sleep_sec;
} display_snapshot_t;

#ifdef __cplusplus
static_assert(sizeof(display_snapshot_t) <= DISPLAY_PROTO_MAX_PAYLOAD,
              "display snapshot exceeds protocol payload");
#else
_Static_assert(sizeof(display_snapshot_t) <= DISPLAY_PROTO_MAX_PAYLOAD,
               "display snapshot exceeds protocol payload");
#endif

typedef struct __attribute__((packed)) {
    uint16_t request_seq;
    uint8_t status;
    uint8_t command;
} display_ack_t;

typedef struct __attribute__((packed)) {
    uint8_t action; /* 0=check, 1=download/install */
    char ssid[33];
    char password[65];
} display_wifi_credentials_t;

typedef struct __attribute__((packed)) {
    uint8_t stage;
    uint8_t result; /* 0=in progress, 1=success, 2=failure */
    char message[62];
} display_hmi_ota_status_t;

typedef struct __attribute__((packed)) {
    uint8_t relay_index;
    uint8_t state;
} display_relay_command_t;

typedef struct __attribute__((packed)) {
    uint8_t loop;
    uint8_t autostart;
    uint8_t step_count;
    display_queue_step_t steps[20];
} display_queue_config_command_t;

typedef struct __attribute__((packed)) {
    uint8_t rule_count;
    display_rule_t rules[16];
} display_automation_config_command_t;

typedef struct {
    uint8_t type;
    uint8_t flags;
    uint16_t sequence;
    uint16_t length;
    uint8_t payload[DISPLAY_PROTO_MAX_PAYLOAD];
} display_frame_t;

typedef struct {
    uint8_t data[DISPLAY_PROTO_FRAME_MAX];
    uint16_t used;
    uint32_t crc_errors;
    uint32_t framing_errors;
} display_parser_t;

typedef void (*display_frame_callback_t)(const display_frame_t *frame, void *context);

uint16_t display_crc16(const uint8_t *data, size_t length);
size_t display_encode_frame(uint8_t type, uint8_t flags, uint16_t sequence,
                            const void *payload, uint16_t length,
                            uint8_t *output, size_t output_size);
void display_parser_init(display_parser_t *parser);
void display_parser_push(display_parser_t *parser, const uint8_t *data, size_t length,
                         display_frame_callback_t callback, void *context);
bool display_protocol_self_test(void);

#ifdef __cplusplus
}
#endif

#endif
