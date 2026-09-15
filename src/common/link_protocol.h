#ifndef SHUNYA_LINK_PROTOCOL_H
#define SHUNYA_LINK_PROTOCOL_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LINK_START_BYTE   0xAAu
#define LINK_END_BYTE     0x55u
#define LINK_MAX_PAYLOAD  48
#define LINK_MAX_FRAME    (3 + LINK_MAX_PAYLOAD + 2)

/* S3 -> hub */
#define HUB_CMD_PING        0x10u
#define HUB_CMD_GET_STATUS  0x11u
#define HUB_CMD_PAIR_OPEN   0x12u
#define HUB_CMD_PAIR_CLOSE  0x13u
#define HUB_CMD_FORGET_NODE 0x14u
#define HUB_CMD_FORGET_ALL  0x15u
#define HUB_CMD_LIST_NODES  0x16u
#define HUB_CMD_ECHO        0x17u
#define HUB_CMD_SET_SLEEP   0x18u

/* hub -> S3 */
#define HUB_EVT_SENSOR      0x20u
#define HUB_EVT_JOINED      0x21u
#define HUB_EVT_LEFT        0x22u
#define HUB_EVT_PAIR_STATE  0x23u
#define HUB_EVT_STATUS      0x24u
#define HUB_EVT_ACK         0x25u
#define HUB_EVT_NACK        0x26u
#define HUB_EVT_NODE_INFO   0x27u
#define HUB_EVT_ECHO        0x28u

#define HUB_MAX_NODES 16

typedef struct {
    uint8_t node_id;
    uint8_t ieee[8];
    uint16_t short_addr;
    float temperature;
    float humidity;
    uint8_t lqi;
} hub_sensor_payload_t;

typedef struct {
    uint8_t node_id;
    uint8_t ieee[8];
    uint16_t short_addr;
    uint8_t lqi;
} hub_node_event_t;

typedef struct {
    uint8_t open;
    uint16_t remaining_sec;
} hub_pair_state_t;

typedef struct {
    uint16_t pan_id;
    uint8_t channel;
    uint8_t node_count;
    uint32_t uptime_ms;
} hub_status_t;

static inline uint8_t link_checksum(const uint8_t *data, uint8_t length) {
    uint8_t checksum = 0;
    for (uint8_t i = 0; i < length; i++) {
        checksum ^= data[i];
    }
    return checksum;
}

/* Returns frame length, or 0 on error. */
static inline uint8_t link_build_frame(uint8_t cmd, const uint8_t *payload, uint8_t length, uint8_t *out, uint8_t out_max) {
    if (length > LINK_MAX_PAYLOAD) {
        return 0;
    }
    uint8_t total = (uint8_t)(3 + length + 2);
    if (out_max < total) {
        return 0;
    }
    out[0] = LINK_START_BYTE;
    out[1] = cmd;
    out[2] = length;
    if (length > 0 && payload != NULL) {
        memcpy(&out[3], payload, length);
    }
    out[3 + length] = link_checksum(&out[1], (uint8_t)(length + 2));
    out[4 + length] = LINK_END_BYTE;
    return total;
}

#ifdef __cplusplus
}
#endif

#endif
