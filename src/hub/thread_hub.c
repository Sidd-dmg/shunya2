#include "thread_hub.h"

#include <string.h>
#include "esp_check.h"
#include "esp_log.h"
#include "esp_openthread.h"
#include "esp_openthread_lock.h"
#include "esp_openthread_types.h"
#include "esp_timer.h"
#include "esp_vfs_eventfd.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hub_config.h"
#include "hub_uart.h"
#include "link_protocol.h"
#include "openthread/ip6.h"
#include "openthread/link.h"
#include "openthread/message.h"
#include "openthread/thread.h"
#include "openthread/thread_ftd.h"
#include "openthread/udp.h"
#include "slots.h"
#include "thread_codec.h"
#include "thread_dataset.h"

static const char *TAG = "thread_hub";
static bool s_pair_open;
static int64_t s_pair_until_us;
static esp_timer_handle_t s_pair_timer;
static otUdpSocket s_socket;
static uint16_t s_last_sequence[HUB_MAX_NODES];
static bool s_have_sequence[HUB_MAX_NODES];

static void pair_timer_callback(void *arg)
{
    (void)arg;
    if (!s_pair_open) {
        return;
    }
    s_pair_open = false;
    s_pair_until_us = 0;
    const uint8_t state[3] = {0, 0, 0};
    hub_uart_send(HUB_EVT_PAIR_STATE, state, sizeof(state));
    ESP_LOGI(TAG, "application admission window closed");
}

static void expire_pair_window(void)
{
    if (s_pair_open && esp_timer_get_time() >= s_pair_until_us) {
        pair_timer_callback(NULL);
    }
}

bool thread_hub_pair_is_open(void)
{
    expire_pair_window();
    return s_pair_open;
}

uint16_t thread_hub_pair_remaining(void)
{
    if (!thread_hub_pair_is_open()) {
        return 0;
    }
    int64_t seconds = (s_pair_until_us - esp_timer_get_time()) / 1000000;
    return (uint16_t)(seconds > 0 ? seconds : 0);
}

uint16_t thread_hub_pan_id(void)
{
    return thread_dataset_pan_id();
}

uint8_t thread_hub_channel(void)
{
    return thread_dataset_channel();
}

void thread_hub_pair_open(uint16_t duration_sec)
{
    if (duration_sec == 0) {
        duration_sec = HUB_PAIR_DEFAULT_S;
    }
    s_pair_open = true;
    s_pair_until_us = esp_timer_get_time() + (int64_t)duration_sec * 1000000;
    if (s_pair_timer) {
        esp_timer_stop(s_pair_timer);
        ESP_ERROR_CHECK(esp_timer_start_once(s_pair_timer, (uint64_t)duration_sec * 1000000));
    }
    ESP_LOGI(TAG, "application admission open for %u seconds", duration_sec);
}

void thread_hub_pair_close(void)
{
    s_pair_open = false;
    s_pair_until_us = 0;
    if (s_pair_timer) {
        esp_timer_stop(s_pair_timer);
    }
    ESP_LOGI(TAG, "application admission closed");
}

void thread_hub_forget_node(uint8_t node_id, const uint8_t *eui64)
{
    int idx = eui64 ? slots_find_ieee(eui64) : -1;
    if (idx < 0 && node_id >= 1 && node_id <= HUB_MAX_NODES) {
        hub_slot_t *slots = slots_table();
        for (int i = 0; i < HUB_MAX_NODES; ++i) {
            if (slots[i].used && slots[i].node_id == node_id) {
                idx = i;
                break;
            }
        }
    }
    if (idx >= 0) {
        hub_slot_t copy = slots_table()[idx];
        slots_free_index(idx);
        s_have_sequence[idx] = false;
        hub_uart_send_left(&copy);
    }
}

void thread_hub_forget_all(void)
{
    hub_slot_t *slots = slots_table();
    for (int i = 0; i < HUB_MAX_NODES; ++i) {
        if (slots[i].used) {
            hub_uart_send_left(&slots[i]);
        }
    }
    slots_clear_all();
    memset(s_have_sequence, 0, sizeof(s_have_sequence));
}

static void send_ack(otInstance *instance, const otMessageInfo *rx_info,
                     const thread_packet_t *received)
{
    thread_packet_t ack = {
        .type = THREAD_MSG_ACK,
        .sequence = received->sequence,
        .ack_sequence = received->sequence,
    };
    uint8_t wire[THREAD_CODEC_WIRE_SIZE];
    if (!thread_codec_encode(&ack, wire, sizeof(wire))) {
        return;
    }
    otMessage *message = otUdpNewMessage(instance, NULL);
    if (!message || otMessageAppend(message, wire, sizeof(wire)) != OT_ERROR_NONE) {
        if (message) {
            otMessageFree(message);
        }
        return;
    }
    otMessageInfo tx_info;
    memset(&tx_info, 0, sizeof(tx_info));
    tx_info.mPeerAddr = rx_info->mPeerAddr;
    tx_info.mPeerPort = rx_info->mPeerPort;
    if (otUdpSend(instance, &s_socket, message, &tx_info) != OT_ERROR_NONE) {
        otMessageFree(message);
    }
}

static void udp_receive(void *context, otMessage *message, const otMessageInfo *info)
{
    otInstance *instance = context;
    uint8_t wire[THREAD_CODEC_WIRE_SIZE];
    thread_packet_t packet;
    if (otMessageGetLength(message) != sizeof(wire) ||
        otMessageRead(message, 0, wire, sizeof(wire)) != sizeof(wire) ||
        !thread_codec_decode(wire, sizeof(wire), &packet)) {
        return;
    }

    int idx = slots_find_ieee(packet.eui64);
    if (packet.type == THREAD_MSG_ADMIT) {
        if (idx < 0) {
            if (!thread_hub_pair_is_open()) {
                ESP_LOGW(TAG, "ignored admission request outside pair window");
                return;
            }
            idx = slots_alloc(packet.eui64, packet.rloc16);
            if (idx < 0) {
                ESP_LOGW(TAG, "ignored admission request: slot table full");
                return;
            }
            hub_uart_send_joined(&slots_table()[idx]);
        } else {
            slots_alloc(packet.eui64, packet.rloc16);
        }
        send_ack(instance, info, &packet);
        return;
    }

    if (packet.type != THREAD_MSG_REPORT || idx < 0) {
        return; /* Reports never create slots: admission is an explicit operation. */
    }

    send_ack(instance, info, &packet);
    if (!thread_sequence_accept(packet.sequence, &s_last_sequence[idx],
                                &s_have_sequence[idx])) {
        return; /* Retry was acknowledged but not forwarded twice. */
    }
    uint8_t lqi = 0;
    otThreadLinkInfo link_info;
    if (otMessageGetThreadLinkInfo(message, &link_info) == OT_ERROR_NONE) {
        lqi = link_info.mLqi;
    }
    slots_table()[idx].short_addr = packet.rloc16;
    slots_update_sensor(idx, packet.temperature_x100 / 100.0f,
                        packet.humidity_x100 / 100.0f, lqi);
    hub_uart_send_sensor(&slots_table()[idx]);
}

static void thread_task(void *arg)
{
    (void)arg;
    const esp_openthread_platform_config_t config = {
        .radio_config = {.radio_mode = RADIO_MODE_NATIVE},
        .host_config = {.host_connection_mode = HOST_CONNECTION_MODE_NONE},
        .port_config = {
            .storage_partition_name = "nvs",
            .netif_queue_size = 10,
            .task_queue_size = 10,
        },
    };
    ESP_ERROR_CHECK(esp_openthread_init(&config));
    otInstance *instance = esp_openthread_get_instance();
    esp_openthread_lock_acquire(portMAX_DELAY);
    ESP_ERROR_CHECK(thread_dataset_apply(instance) == OT_ERROR_NONE ? ESP_OK : ESP_FAIL);
    ESP_ERROR_CHECK(otThreadSetRouterEligible(instance, true) == OT_ERROR_NONE ? ESP_OK : ESP_FAIL);
    ESP_ERROR_CHECK(otIp6SetEnabled(instance, true) == OT_ERROR_NONE ? ESP_OK : ESP_FAIL);

    otNetifAddress service;
    memset(&service, 0, sizeof(service));
    ESP_ERROR_CHECK(otIp6AddressFromString(THREAD_COLLECTOR_ADDRESS, &service.mAddress) == OT_ERROR_NONE
                        ? ESP_OK : ESP_FAIL);
    service.mPrefixLength = 64;
    service.mPreferred = true;
    service.mValid = true;
    ESP_ERROR_CHECK(otIp6AddUnicastAddress(instance, &service) == OT_ERROR_NONE ? ESP_OK : ESP_FAIL);

    memset(&s_socket, 0, sizeof(s_socket));
    ESP_ERROR_CHECK(otUdpOpen(instance, &s_socket, udp_receive, instance) == OT_ERROR_NONE ? ESP_OK : ESP_FAIL);
    otSockAddr bind_addr;
    memset(&bind_addr, 0, sizeof(bind_addr));
    bind_addr.mPort = THREAD_SERVICE_PORT;
    ESP_ERROR_CHECK(otUdpBind(instance, &s_socket, &bind_addr, OT_NETIF_THREAD_INTERNAL) == OT_ERROR_NONE
                        ? ESP_OK : ESP_FAIL);
    ESP_ERROR_CHECK(otThreadSetEnabled(instance, true) == OT_ERROR_NONE ? ESP_OK : ESP_FAIL);
    esp_openthread_lock_release();

    ESP_LOGI(TAG, "collector listening at [%s]:%u", THREAD_COLLECTOR_ADDRESS, THREAD_SERVICE_PORT);
    ESP_ERROR_CHECK(esp_openthread_launch_mainloop());
    vTaskDelete(NULL);
}

void thread_hub_start(void)
{
    const esp_vfs_eventfd_config_t eventfd = {.max_fds = 3};
    ESP_ERROR_CHECK(esp_vfs_eventfd_register(&eventfd));
    const esp_timer_create_args_t timer_args = {
        .callback = pair_timer_callback,
        .name = "pair_window",
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &s_pair_timer));
    xTaskCreate(thread_task, "thread_hub", 8192, NULL, 5, NULL);
}
