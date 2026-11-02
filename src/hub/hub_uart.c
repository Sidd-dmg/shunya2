#include "hub_uart.h"
#include "hub_config.h"
#include "link_protocol.h"
#include "slots.h"
#include "zigbee_coord.h"
#include "shunya_zb.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>

static const char *TAG = "hub_uart";
static SemaphoreHandle_t s_tx_mux;

void hub_uart_init(void) {
    s_tx_mux = xSemaphoreCreateMutex();
    uart_config_t cfg = {
        .baud_rate = HUB_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    uart_driver_install(HUB_UART_NUM, HUB_UART_BUF * 2, HUB_UART_BUF, 0, NULL, 0);
    uart_param_config(HUB_UART_NUM, &cfg);
    uart_set_pin(HUB_UART_NUM, HUB_UART_TX_GPIO, HUB_UART_RX_GPIO, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
}

void hub_uart_send(uint8_t cmd, const uint8_t *payload, uint8_t length) {
    uint8_t frame[LINK_MAX_FRAME];
    uint8_t n = link_build_frame(cmd, payload, length, frame, sizeof(frame));
    if (n == 0 || s_tx_mux == NULL) {
        return;
    }
    if (xSemaphoreTake(s_tx_mux, pdMS_TO_TICKS(100)) == pdTRUE) {
        uart_write_bytes(HUB_UART_NUM, (const char *)frame, n);
        xSemaphoreGive(s_tx_mux);
    }
}

static void pack_node_event(const hub_slot_t *s, uint8_t *p) {
    p[0] = s->node_id;
    memcpy(&p[1], s->ieee, 8);
    p[9] = (uint8_t)(s->short_addr & 0xFF);
    p[10] = (uint8_t)(s->short_addr >> 8);
    p[11] = s->lqi;
}

static void send_status(void) {
    uint8_t p[8];
    uint16_t pan = zigbee_coord_pan_id();
    uint32_t up = (uint32_t)(esp_timer_get_time() / 1000);
    p[0] = (uint8_t)(pan & 0xFF);
    p[1] = (uint8_t)(pan >> 8);
    p[2] = zigbee_coord_channel();
    p[3] = (uint8_t)slots_count();
    memcpy(&p[4], &up, 4);
    hub_uart_send(HUB_EVT_STATUS, p, 8);
}

static void send_pair_state(void) {
    uint8_t p[3];
    uint16_t rem = zigbee_coord_pair_remaining();
    p[0] = zigbee_coord_pair_is_open() ? 1 : 0;
    p[1] = (uint8_t)(rem & 0xFF);
    p[2] = (uint8_t)(rem >> 8);
    hub_uart_send(HUB_EVT_PAIR_STATE, p, 3);
}

static void send_node_list(void) {
    hub_slot_t *t = slots_table();
    for (int i = 0; i < HUB_MAX_NODES; i++) {
        if (!t[i].used) {
            continue;
        }
        uint8_t p[21];
        pack_node_event(&t[i], p);
        memcpy(&p[12], &t[i].last_temp, 4);
        memcpy(&p[16], &t[i].last_hum, 4);
        p[20] = t[i].battery_pct;
        hub_uart_send(HUB_EVT_NODE_INFO, p, 21);
    }
}

static void handle_cmd(uint8_t cmd, const uint8_t *payload, uint8_t length) {
    switch (cmd) {
        case HUB_CMD_PING:
            hub_uart_send(HUB_EVT_ACK, &cmd, 1);
            ESP_LOGI(TAG, "ping");
            break;
        case HUB_CMD_GET_STATUS:
            send_status();
            break;
        case HUB_CMD_PAIR_OPEN: {
            uint16_t dur = HUB_PAIR_DEFAULT_S;
            if (length >= 2) {
                dur = (uint16_t)payload[0] | ((uint16_t)payload[1] << 8);
            }
            ESP_LOGI(TAG, "pair open request, %u s", dur);
            zigbee_coord_pair_open(dur);
            send_pair_state();
            break;
        }
        case HUB_CMD_PAIR_CLOSE:
            ESP_LOGI(TAG, "pair close request");
            zigbee_coord_pair_close();
            send_pair_state();
            break;
        case HUB_CMD_FORGET_NODE: {
            uint8_t node_id = (length >= 1) ? payload[0] : 0;
            const uint8_t *ieee = (length >= 9) ? &payload[1] : NULL;
            zigbee_coord_forget_node(node_id, ieee);
            hub_uart_send(HUB_EVT_ACK, &cmd, 1);
            break;
        }
        case HUB_CMD_FORGET_ALL:
            zigbee_coord_forget_all();
            hub_uart_send(HUB_EVT_ACK, &cmd, 1);
            break;
        case HUB_CMD_LIST_NODES:
            send_node_list();
            break;
        case HUB_CMD_ECHO:
            hub_uart_send(HUB_EVT_ECHO, payload, length);
            ESP_LOGI(TAG, "echo %u bytes", (unsigned)length);
            break;
        case HUB_CMD_SET_SLEEP: {
            uint16_t seconds = SENSOR_SLEEP_DEFAULT_S;
            if (length >= 2) {
                seconds = (uint16_t)payload[0] | ((uint16_t)payload[1] << 8);
            }
            zigbee_coord_set_sleep_sec(seconds);
            hub_uart_send(HUB_EVT_ACK, &cmd, 1);
            break;
        }
        default:
            hub_uart_send(HUB_EVT_NACK, &cmd, 1);
            break;
    }
}

void hub_uart_task(void *arg) {
    (void)arg;
    uint8_t rx[96];
    int count = 0;
    for (;;) {
        uint8_t b;
        int n = uart_read_bytes(HUB_UART_NUM, &b, 1, pdMS_TO_TICKS(20));
        if (n == 1 && count < (int)sizeof(rx)) {
            rx[count++] = b;
        }
        int i = 0;
        while (i < count) {
            if (rx[i] != LINK_START_BYTE) {
                i++;
                continue;
            }
            if (i + 3 > count) {
                break;
            }
            uint8_t cmd = rx[i + 1];
            uint8_t length = rx[i + 2];
            int packet_size = 3 + length + 2;
            if (i + packet_size > count) {
                break;
            }
            if (rx[i + packet_size - 1] != LINK_END_BYTE) {
                i++;
                continue;
            }
            if (rx[i + packet_size - 2] != link_checksum(&rx[i + 1], (uint8_t)(length + 2))) {
                i++;
                continue;
            }
            handle_cmd(cmd, &rx[i + 3], length);
            i += packet_size;
        }
        if (i > 0) {
            memmove(rx, &rx[i], count - i);
            count -= i;
        }
    }
}

void hub_uart_send_sensor(const hub_slot_t *s) {
    uint8_t p[23];
    p[0] = s->node_id;
    memcpy(&p[1], s->ieee, 8);
    p[9] = (uint8_t)(s->short_addr & 0xFF);
    p[10] = (uint8_t)(s->short_addr >> 8);
    memcpy(&p[11], &s->last_temp, 4);
    memcpy(&p[15], &s->last_hum, 4);
    p[19] = s->lqi;
    p[20] = s->battery_pct;
    uint16_t sleep = zigbee_coord_sleep_sec();
    p[21] = (uint8_t)(sleep & 0xFF);
    p[22] = (uint8_t)(sleep >> 8);
    hub_uart_send(HUB_EVT_SENSOR, p, 23);
}

void hub_uart_send_joined(const hub_slot_t *s) {
    uint8_t p[12];
    pack_node_event(s, p);
    hub_uart_send(HUB_EVT_JOINED, p, 12);
}

void hub_uart_send_left(const hub_slot_t *s) {
    uint8_t p[12];
    pack_node_event(s, p);
    hub_uart_send(HUB_EVT_LEFT, p, 12);
}
