#include "thread_sensor.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_check.h"
#include "esp_log.h"
#include "esp_openthread.h"
#include "esp_openthread_lock.h"
#include "esp_openthread_types.h"
#include "esp_vfs_eventfd.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "openthread/ip6.h"
#include "openthread/link.h"
#include "openthread/message.h"
#include "openthread/thread.h"
#include "openthread/thread_ftd.h"
#include "openthread/udp.h"
#include "sensor_config.h"
#include "thread_codec.h"
#include "thread_dataset.h"
#ifndef SENSOR_DEMO_SERIAL
#include "sht40.h"
#endif

static const char *TAG = "thread_sensor";
static otInstance *s_instance;
static otUdpSocket s_socket;
static TaskHandle_t s_report_task;
static volatile bool s_attached;
static volatile bool s_admitted;
static volatile uint16_t s_waiting_ack;
static volatile uint16_t s_received_ack;
static uint16_t s_sequence;
static uint8_t s_eui64[THREAD_CODEC_EUI64_SIZE];

#ifdef SENSOR_DEMO_SERIAL
static portMUX_TYPE s_demo_lock = portMUX_INITIALIZER_UNLOCKED;
static int16_t s_demo_temp_x100;
static uint16_t s_demo_hum_x100;
static bool s_demo_has_value;
#endif

static void udp_receive(void *context, otMessage *message, const otMessageInfo *info)
{
    (void)context;
    (void)info;
    uint8_t wire[THREAD_CODEC_WIRE_SIZE];
    thread_packet_t packet;
    if (otMessageGetLength(message) == sizeof(wire) &&
        otMessageRead(message, 0, wire, sizeof(wire)) == sizeof(wire) &&
        thread_codec_decode(wire, sizeof(wire), &packet) &&
        packet.type == THREAD_MSG_ACK && packet.ack_sequence == s_waiting_ack) {
        s_received_ack = packet.ack_sequence;
        if (s_report_task) {
            xTaskNotifyGive(s_report_task);
        }
    }
}

static bool send_with_ack(thread_msg_type_t type, int16_t temp_x100, uint16_t hum_x100)
{
    thread_packet_t packet;
    memset(&packet, 0, sizeof(packet));
    packet.type = type;
    packet.sequence = ++s_sequence;
    memcpy(packet.eui64, s_eui64, sizeof(packet.eui64));
    packet.temperature_x100 = temp_x100;
    packet.humidity_x100 = hum_x100;
    s_waiting_ack = packet.sequence;
    s_received_ack = 0;

    for (int attempt = 0; attempt < 3 && s_attached; ++attempt) {
        esp_openthread_lock_acquire(portMAX_DELAY);
        packet.rloc16 = otThreadGetRloc16(s_instance);
        uint8_t wire[THREAD_CODEC_WIRE_SIZE];
        otMessage *message = NULL;
        otError error = thread_codec_encode(&packet, wire, sizeof(wire)) ? OT_ERROR_NONE : OT_ERROR_INVALID_ARGS;
        if (error == OT_ERROR_NONE) {
            message = otUdpNewMessage(s_instance, NULL);
            error = message ? otMessageAppend(message, wire, sizeof(wire)) : OT_ERROR_NO_BUFS;
        }
        otMessageInfo info;
        memset(&info, 0, sizeof(info));
        if (error == OT_ERROR_NONE) {
            error = otIp6AddressFromString(THREAD_COLLECTOR_ADDRESS, &info.mPeerAddr);
            info.mPeerPort = THREAD_SERVICE_PORT;
        }
        if (error == OT_ERROR_NONE) {
            error = otUdpSend(s_instance, &s_socket, message, &info);
        }
        if (error != OT_ERROR_NONE && message) {
            otMessageFree(message);
        }
        esp_openthread_lock_release();
        if (error != OT_ERROR_NONE) {
            ESP_LOGW(TAG, "UDP send failed: %s", otThreadErrorToString(error));
        }
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1200));
        if (s_received_ack == packet.sequence) {
            return true;
        }
    }
    return false;
}

static void state_changed(otChangedFlags flags, void *context)
{
    otInstance *instance = context;
    if (flags & OT_CHANGED_THREAD_ROLE) {
        const otDeviceRole role = otThreadGetDeviceRole(instance);
        s_attached = role == OT_DEVICE_ROLE_CHILD || role == OT_DEVICE_ROLE_ROUTER ||
                     role == OT_DEVICE_ROLE_LEADER;
        if (!s_attached) {
            s_admitted = false;
        }
        if (s_report_task) {
            xTaskNotifyGive(s_report_task);
        }
        ESP_LOGI(TAG, "Thread role %d", role);
    }
}

#ifdef SENSOR_DEMO_SERIAL
static bool parse_demo_values(char *line, float *temperature, float *humidity)
{
    char *cursor = line;
    while (isspace((unsigned char)*cursor)) ++cursor;
    if (strncmp(cursor, "set", 3) == 0 && isspace((unsigned char)cursor[3])) cursor += 3;
    char *end = NULL;
    const float t = strtof(cursor, &end);
    if (end == cursor) return false;
    cursor = end;
    const float h = strtof(cursor, &end);
    if (end == cursor) return false;
    while (isspace((unsigned char)*end)) ++end;
    if (*end || !isfinite(t) || !isfinite(h) || t < -40 || t > 80 || h < 0 || h > 100) return false;
    *temperature = t;
    *humidity = h;
    return true;
}

static void demo_serial_task(void *arg)
{
    (void)arg;
    char line[96];
    size_t used = 0;
    ESP_LOGI(TAG, "Serial input ready. Type: set 24.5 55.0");
    for (;;) {
        const int input = getchar();
        if (input < 0) {
            clearerr(stdin);
            vTaskDelay(pdMS_TO_TICKS(20));
        } else if (input == '\b' || input == 0x7f) {
            if (used) --used;
        } else if (input != '\r' && input != '\n') {
            if (isprint((unsigned char)input) && used + 1 < sizeof(line)) line[used++] = (char)input;
        } else if (used) {
            line[used] = '\0';
            used = 0;
            float t, h;
            if (!parse_demo_values(line, &t, &h)) {
                ESP_LOGW(TAG, "Invalid input. Use: set <temp_C> <humidity_%%>");
                continue;
            }
            taskENTER_CRITICAL(&s_demo_lock);
            s_demo_temp_x100 = (int16_t)lroundf(t * 100.0f);
            s_demo_hum_x100 = (uint16_t)lroundf(h * 100.0f);
            s_demo_has_value = true;
            taskEXIT_CRITICAL(&s_demo_lock);
            if (s_report_task) xTaskNotifyGive(s_report_task);
        }
    }
}
#endif

static void report_task(void *arg)
{
    (void)arg;
    s_report_task = xTaskGetCurrentTaskHandle();
    vTaskDelay(pdMS_TO_TICKS(3000));
    for (;;) {
        if (!s_attached) {
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(2000));
            continue;
        }
        if (!s_admitted) {
            s_admitted = send_with_ack(THREAD_MSG_ADMIT, 0, 0);
            if (!s_admitted) {
                ESP_LOGW(TAG, "collector has not admitted this device; open pairing");
                vTaskDelay(pdMS_TO_TICKS(3000));
                continue;
            }
            ESP_LOGI(TAG, "application admission confirmed");
        }

        int16_t temp_x100 = 0;
        uint16_t hum_x100 = 0;
        bool have_value = false;
#ifdef SENSOR_DEMO_SERIAL
        taskENTER_CRITICAL(&s_demo_lock);
        temp_x100 = s_demo_temp_x100;
        hum_x100 = s_demo_hum_x100;
        have_value = s_demo_has_value;
        taskEXIT_CRITICAL(&s_demo_lock);
#else
        float t = 0, h = 0;
        esp_err_t error = ESP_FAIL;
        for (int attempt = 0; attempt < 3 && error != ESP_OK; ++attempt) {
            error = sht40_read(&t, &h);
            if (error != ESP_OK) vTaskDelay(pdMS_TO_TICKS(50));
        }
        if (error == ESP_OK) {
            temp_x100 = (int16_t)lroundf(t * 100.0f);
            hum_x100 = (uint16_t)lroundf(h * 100.0f);
            have_value = true;
        } else {
            ESP_LOGW(TAG, "SHT40 read failed: %s", esp_err_to_name(error));
        }
#endif
        if (have_value && !send_with_ack(THREAD_MSG_REPORT, temp_x100, hum_x100)) {
            ESP_LOGW(TAG, "report acknowledgement timed out; admission will be renewed");
            s_admitted = false;
        }
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(REPORT_INTERVAL_MS));
    }
}

static void thread_task(void *arg)
{
    (void)arg;
    const esp_openthread_platform_config_t config = {
        .radio_config = {.radio_mode = RADIO_MODE_NATIVE},
        .host_config = {.host_connection_mode = HOST_CONNECTION_MODE_NONE},
        .port_config = {.storage_partition_name = "nvs", .netif_queue_size = 10, .task_queue_size = 10},
    };
    ESP_ERROR_CHECK(esp_openthread_init(&config));
    s_instance = esp_openthread_get_instance();
    esp_openthread_lock_acquire(portMAX_DELAY);
    ESP_ERROR_CHECK(thread_dataset_apply(s_instance) == OT_ERROR_NONE ? ESP_OK : ESP_FAIL);
    otExtAddress eui;
    otLinkGetFactoryAssignedIeeeEui64(s_instance, &eui);
    memcpy(s_eui64, eui.m8, sizeof(s_eui64));
    ESP_ERROR_CHECK(otThreadSetRouterEligible(s_instance, true) == OT_ERROR_NONE ? ESP_OK : ESP_FAIL);
    ESP_ERROR_CHECK(otSetStateChangedCallback(s_instance, state_changed, s_instance) == OT_ERROR_NONE ? ESP_OK : ESP_FAIL);
    ESP_ERROR_CHECK(otIp6SetEnabled(s_instance, true) == OT_ERROR_NONE ? ESP_OK : ESP_FAIL);
    ESP_ERROR_CHECK(otUdpOpen(s_instance, &s_socket, udp_receive, s_instance) == OT_ERROR_NONE ? ESP_OK : ESP_FAIL);
    otSockAddr local;
    memset(&local, 0, sizeof(local));
    ESP_ERROR_CHECK(otUdpBind(s_instance, &s_socket, &local, OT_NETIF_THREAD_INTERNAL) == OT_ERROR_NONE ? ESP_OK : ESP_FAIL);
    ESP_ERROR_CHECK(otThreadSetEnabled(s_instance, true) == OT_ERROR_NONE ? ESP_OK : ESP_FAIL);
    esp_openthread_lock_release();
    ESP_ERROR_CHECK(esp_openthread_launch_mainloop());
    vTaskDelete(NULL);
}

void thread_sensor_start(void)
{
    const esp_vfs_eventfd_config_t eventfd = {.max_fds = 3};
    ESP_ERROR_CHECK(esp_vfs_eventfd_register(&eventfd));
    xTaskCreate(report_task, "sensor_report", 6144, NULL, 3, NULL);
#ifdef SENSOR_DEMO_SERIAL
    xTaskCreate(demo_serial_task, "demo_serial", 4096, NULL, 2, NULL);
#endif
    xTaskCreate(thread_task, "thread_sensor", 8192, NULL, 5, NULL);
}

void thread_sensor_factory_reset(void)
{
    if (!s_instance) return;
    esp_openthread_lock_acquire(portMAX_DELAY);
    otInstanceFactoryReset(s_instance);
    esp_openthread_lock_release();
}
