#include "zigbee_coord.h"

#include <string.h>
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ha/esp_zigbee_ha_standard.h"
#include "hub_config.h"
#include "hub_uart.h"
#include "link_protocol.h"
#include "nvs.h"
#include "shunya_zb.h"
#include "slots.h"

static const char *TAG = "zb_coord";
static bool s_pair_open;
static int64_t s_pair_until_us;
static esp_timer_handle_t s_pair_timer;
static uint16_t s_sleep_sec = SENSOR_SLEEP_DEFAULT_S;
static uint16_t s_pan_id;
static uint8_t s_channel;

static void load_sleep(void)
{
    nvs_handle_t nvs;
    if (nvs_open(HUB_NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return;
    }
    uint16_t stored = SENSOR_SLEEP_DEFAULT_S;
    if (nvs_get_u16(nvs, "sleep", &stored) == ESP_OK) {
        s_sleep_sec = shunya_clamp_sleep_sec(stored);
    }
    nvs_close(nvs);
}

static void save_sleep(void)
{
    nvs_handle_t nvs;
    if (nvs_open(HUB_NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) {
        return;
    }
    nvs_set_u16(nvs, "sleep", s_sleep_sec);
    nvs_commit(nvs);
    nvs_close(nvs);
}

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
    ESP_LOGI(TAG, "permit-join window closed");
}

static void expire_pair_window(void)
{
    if (s_pair_open && esp_timer_get_time() >= s_pair_until_us) {
        pair_timer_callback(NULL);
    }
}

bool zigbee_coord_pair_is_open(void)
{
    expire_pair_window();
    return s_pair_open;
}

uint16_t zigbee_coord_pair_remaining(void)
{
    if (!zigbee_coord_pair_is_open()) {
        return 0;
    }
    int64_t seconds = (s_pair_until_us - esp_timer_get_time()) / 1000000;
    return (uint16_t)(seconds > 0 ? seconds : 0);
}

uint16_t zigbee_coord_pan_id(void)
{
    return s_pan_id ? s_pan_id : esp_zb_get_pan_id();
}

uint8_t zigbee_coord_channel(void)
{
    return s_channel ? s_channel : (uint8_t)esp_zb_get_current_channel();
}

uint16_t zigbee_coord_sleep_sec(void)
{
    return s_sleep_sec;
}

static void send_sleep_write(uint16_t short_addr)
{
    uint16_t value = s_sleep_sec;
    esp_zb_zcl_attribute_t attr = {
        .id = SHUNYA_ZB_ATTR_SLEEP_SEC,
        .data = {
            .type = ESP_ZB_ZCL_ATTR_TYPE_U16,
            .size = sizeof(value),
            .value = &value,
        },
    };
    esp_zb_zcl_write_attr_cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.zcl_basic_cmd.dst_addr_u.addr_short = short_addr;
    cmd.zcl_basic_cmd.dst_endpoint = SHUNYA_ZB_ENDPOINT;
    cmd.zcl_basic_cmd.src_endpoint = SHUNYA_ZB_ENDPOINT;
    cmd.address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;
    cmd.clusterID = SHUNYA_ZB_CLUSTER_ID;
    cmd.direction = ESP_ZB_ZCL_CMD_DIRECTION_TO_SRV;
    cmd.attr_number = 1;
    cmd.attr_field = &attr;
    esp_zb_zcl_write_attr_cmd_req(&cmd);
}

static void broadcast_sleep(void)
{
    hub_slot_t *slots = slots_table();
    for (int i = 0; i < HUB_MAX_NODES; ++i) {
        if (slots[i].used && slots[i].short_addr) {
            send_sleep_write(slots[i].short_addr);
        }
    }
}

void zigbee_coord_pair_open(uint16_t duration_sec)
{
    if (duration_sec == 0) {
        duration_sec = HUB_PAIR_DEFAULT_S;
    }
    if (duration_sec > 254) {
        duration_sec = 254;
    }
    s_pair_open = true;
    s_pair_until_us = esp_timer_get_time() + (int64_t)duration_sec * 1000000;
    if (s_pair_timer) {
        esp_timer_stop(s_pair_timer);
        ESP_ERROR_CHECK(esp_timer_start_once(s_pair_timer, (uint64_t)duration_sec * 1000000));
    }
    esp_zb_lock_acquire(portMAX_DELAY);
    esp_zb_bdb_open_network((uint8_t)duration_sec);
    esp_zb_lock_release();
    ESP_LOGI(TAG, "permit join %u s", duration_sec);
}

void zigbee_coord_pair_close(void)
{
    s_pair_open = false;
    s_pair_until_us = 0;
    if (s_pair_timer) {
        esp_timer_stop(s_pair_timer);
    }
    esp_zb_lock_acquire(portMAX_DELAY);
    esp_zb_bdb_open_network(0);
    esp_zb_lock_release();
    ESP_LOGI(TAG, "network closed");
}

void zigbee_coord_forget_node(uint8_t node_id, const uint8_t *ieee)
{
    int idx = ieee ? slots_find_ieee(ieee) : -1;
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
        hub_uart_send_left(&copy);
    }
}

void zigbee_coord_forget_all(void)
{
    hub_slot_t *slots = slots_table();
    for (int i = 0; i < HUB_MAX_NODES; ++i) {
        if (slots[i].used) {
            hub_uart_send_left(&slots[i]);
        }
    }
    slots_clear_all();
}

void zigbee_coord_set_sleep_sec(uint16_t seconds)
{
    s_sleep_sec = shunya_clamp_sleep_sec(seconds);
    save_sleep();
    esp_zb_lock_acquire(portMAX_DELAY);
    broadcast_sleep();
    esp_zb_lock_release();
    ESP_LOGI(TAG, "sensor sleep set to %u s", s_sleep_sec);
}

static void handle_join(const uint8_t *ieee, uint16_t short_addr)
{
    int idx = slots_find_ieee(ieee);
    if (idx < 0) {
        if (!zigbee_coord_pair_is_open()) {
            ESP_LOGW(TAG, "ignored join outside permit-join window");
            return;
        }
        idx = slots_alloc(ieee, short_addr);
        if (idx < 0) {
            ESP_LOGW(TAG, "ignored join: slot table full");
            return;
        }
        hub_uart_send_joined(&slots_table()[idx]);
        ESP_LOGI(TAG, "joined node %u short 0x%04X", slots_table()[idx].node_id, short_addr);
    } else {
        slots_alloc(ieee, short_addr);
    }
    send_sleep_write(short_addr);
}

static void handle_report(uint16_t short_addr, uint16_t cluster, uint16_t attr,
                          const uint8_t *value, uint8_t size, uint8_t lqi)
{
    uint8_t ieee[8] = {0};
    esp_zb_ieee_address_by_short(short_addr, ieee);
    int idx = slots_find_ieee(ieee);
    if (idx < 0) {
        idx = slots_find_short(short_addr);
    }
    if (idx < 0) {
        return;
    }
    hub_slot_t *slot = &slots_table()[idx];
    slot->short_addr = short_addr;
    if (lqi) {
        slot->lqi = lqi;
    }
    if (cluster == ESP_ZB_ZCL_CLUSTER_ID_TEMP_MEASUREMENT && size >= 2) {
        int16_t raw = (int16_t)(value[0] | ((uint16_t)value[1] << 8));
        slot->last_temp = raw / 100.0f;
    } else if (cluster == ESP_ZB_ZCL_CLUSTER_ID_REL_HUMIDITY_MEASUREMENT && size >= 2) {
        uint16_t raw = (uint16_t)(value[0] | ((uint16_t)value[1] << 8));
        slot->last_hum = raw / 100.0f;
    } else if (cluster == SHUNYA_ZB_CLUSTER_ID && attr == SHUNYA_ZB_ATTR_BATTERY_PCT && size >= 1) {
        slot->battery_pct = value[0];
    }
    hub_uart_send_sensor(slot);
    send_sleep_write(short_addr);
}

static void bdb_start_top_level_commissioning_cb(uint8_t mode_mask)
{
    ESP_RETURN_ON_FALSE(esp_zb_bdb_start_top_level_commissioning(mode_mask) == ESP_OK, ,
                        TAG, "Failed to start Zigbee commissioning");
}

void esp_zb_app_signal_handler(esp_zb_app_signal_t *signal_struct)
{
    uint32_t *p_sg_p = signal_struct->p_app_signal;
    esp_err_t err_status = signal_struct->esp_err_status;
    esp_zb_app_signal_type_t sig_type = *p_sg_p;
    switch (sig_type) {
    case ESP_ZB_ZDO_SIGNAL_SKIP_STARTUP:
        ESP_LOGI(TAG, "Initialize Zigbee stack");
        esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_INITIALIZATION);
        break;
    case ESP_ZB_BDB_SIGNAL_DEVICE_FIRST_START:
    case ESP_ZB_BDB_SIGNAL_DEVICE_REBOOT:
        if (err_status == ESP_OK) {
            s_pan_id = esp_zb_get_pan_id();
            s_channel = (uint8_t)esp_zb_get_current_channel();
            if (esp_zb_bdb_is_factory_new()) {
                ESP_LOGI(TAG, "Start network formation");
                esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_NETWORK_FORMATION);
            } else {
                ESP_LOGI(TAG, "Existing network PAN 0x%04X ch %u", s_pan_id, s_channel);
            }
        } else {
            ESP_LOGE(TAG, "Failed to initialize Zigbee stack (%s)", esp_err_to_name(err_status));
        }
        break;
    case ESP_ZB_BDB_SIGNAL_FORMATION:
        if (err_status == ESP_OK) {
            s_pan_id = esp_zb_get_pan_id();
            s_channel = (uint8_t)esp_zb_get_current_channel();
            ESP_LOGI(TAG, "Formed network PAN 0x%04X ch %u", s_pan_id, s_channel);
            esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_NETWORK_STEERING);
            zigbee_coord_pair_close();
        } else {
            esp_zb_scheduler_alarm((esp_zb_callback_t)bdb_start_top_level_commissioning_cb,
                                   ESP_ZB_BDB_MODE_NETWORK_FORMATION, 1000);
        }
        break;
    case ESP_ZB_ZDO_SIGNAL_DEVICE_ANNCE: {
        esp_zb_zdo_signal_device_annce_params_t *dev =
            (esp_zb_zdo_signal_device_annce_params_t *)esp_zb_app_signal_get_params(p_sg_p);
        handle_join(dev->ieee_addr, dev->device_short_addr);
        break;
    }
    case ESP_ZB_NWK_SIGNAL_PERMIT_JOIN_STATUS:
        if (err_status == ESP_OK) {
            uint8_t duration = *(uint8_t *)esp_zb_app_signal_get_params(p_sg_p);
            if (duration) {
                ESP_LOGI(TAG, "Network open for joining %u s", duration);
            } else {
                ESP_LOGI(TAG, "Network closed");
            }
        }
        break;
    default:
        ESP_LOGD(TAG, "ZDO signal: %s (0x%x) %s", esp_zb_zdo_signal_to_string(sig_type),
                 sig_type, esp_err_to_name(err_status));
        break;
    }
}

static esp_err_t zb_action_handler(esp_zb_core_action_callback_id_t callback_id, const void *message)
{
    if (callback_id != ESP_ZB_CORE_REPORT_ATTR_CB_ID || !message) {
        return ESP_OK;
    }
    const esp_zb_zcl_report_attr_message_t *report = message;
    if (!report->attribute.data.value) {
        return ESP_OK;
    }
    uint16_t short_addr = report->src_address.u.short_addr;
    uint8_t lqi = 0;
    handle_report(short_addr, report->cluster, report->attribute.id,
                  (const uint8_t *)report->attribute.data.value,
                  report->attribute.data.size, lqi);
    return ESP_OK;
}

static void add_basic_cluster(esp_zb_cluster_list_t *cluster_list)
{
    esp_zb_basic_cluster_cfg_t basic_cfg = {
        .zcl_version = ESP_ZB_ZCL_BASIC_ZCL_VERSION_DEFAULT_VALUE,
        .power_source = ESP_ZB_ZCL_BASIC_POWER_SOURCE_DEFAULT_VALUE,
    };
    esp_zb_attribute_list_t *basic = esp_zb_basic_cluster_create(&basic_cfg);
    char manufacturer[] = "\x06" "Shunya";
    char model[] = "\x0A" "Zigbee-hub";
    esp_zb_basic_cluster_add_attr(basic, ESP_ZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID, manufacturer);
    esp_zb_basic_cluster_add_attr(basic, ESP_ZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID, model);
    esp_zb_cluster_list_add_basic_cluster(cluster_list, basic, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    esp_zb_identify_cluster_cfg_t identify_cfg = {
        .identify_time = ESP_ZB_ZCL_IDENTIFY_IDENTIFY_TIME_DEFAULT_VALUE,
    };
    esp_zb_cluster_list_add_identify_cluster(cluster_list,
                                             esp_zb_identify_cluster_create(&identify_cfg),
                                             ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
}

static esp_zb_cluster_list_t *create_hub_clusters(void)
{
    esp_zb_cluster_list_t *cluster_list = esp_zb_zcl_cluster_list_create();
    add_basic_cluster(cluster_list);

    esp_zb_temperature_meas_cluster_cfg_t temp_cfg = {
        .measured_value = (int16_t)0x8000,
        .min_value = -4000,
        .max_value = 8000,
    };
    esp_zb_cluster_list_add_temperature_meas_cluster(
        cluster_list, esp_zb_temperature_meas_cluster_create(&temp_cfg),
        ESP_ZB_ZCL_CLUSTER_CLIENT_ROLE);

    esp_zb_humidity_meas_cluster_cfg_t hum_cfg = {
        .measured_value = 0xFFFF,
        .min_value = 0,
        .max_value = 10000,
    };
    esp_zb_cluster_list_add_humidity_meas_cluster(
        cluster_list, esp_zb_humidity_meas_cluster_create(&hum_cfg),
        ESP_ZB_ZCL_CLUSTER_CLIENT_ROLE);

    uint16_t sleep = s_sleep_sec;
    uint8_t battery = BATTERY_PCT_UNKNOWN;
    esp_zb_attribute_list_t *custom = esp_zb_zcl_attr_list_create(SHUNYA_ZB_CLUSTER_ID);
    ESP_ERROR_CHECK(esp_zb_cluster_add_attr(custom, SHUNYA_ZB_CLUSTER_ID, SHUNYA_ZB_ATTR_SLEEP_SEC,
                                            ESP_ZB_ZCL_ATTR_TYPE_U16,
                                            ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE, &sleep));
    ESP_ERROR_CHECK(esp_zb_cluster_add_attr(custom, SHUNYA_ZB_CLUSTER_ID, SHUNYA_ZB_ATTR_BATTERY_PCT,
                                            ESP_ZB_ZCL_ATTR_TYPE_U8,
                                            ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY, &battery));
    ESP_ERROR_CHECK(esp_zb_cluster_list_add_custom_cluster(cluster_list, custom,
                                                           ESP_ZB_ZCL_CLUSTER_CLIENT_ROLE));
    return cluster_list;
}

static void zigbee_task(void *arg)
{
    (void)arg;
    esp_zb_cfg_t zb_nwk_cfg = {
        .esp_zb_role = ESP_ZB_DEVICE_TYPE_COORDINATOR,
        .install_code_policy = false,
        .nwk_cfg.zczr_cfg = {
            .max_children = HUB_MAX_NODES,
        },
    };
    esp_zb_init(&zb_nwk_cfg);
    esp_zb_ep_list_t *ep_list = esp_zb_ep_list_create();
    esp_zb_endpoint_config_t ep_config = {
        .endpoint = SHUNYA_ZB_ENDPOINT,
        .app_profile_id = ESP_ZB_AF_HA_PROFILE_ID,
        .app_device_id = ESP_ZB_HA_TEMPERATURE_SENSOR_DEVICE_ID,
        .app_device_version = 0,
    };
    esp_zb_ep_list_add_ep(ep_list, create_hub_clusters(), ep_config);
    esp_zb_device_register(ep_list);
    esp_zb_core_action_handler_register(zb_action_handler);
    esp_zb_set_primary_network_channel_set(ESP_ZB_TRANSCEIVER_ALL_CHANNELS_MASK);
    ESP_ERROR_CHECK(esp_zb_start(false));
    esp_zb_stack_main_loop();
}

void zigbee_coord_start(void)
{
    load_sleep();
    const esp_timer_create_args_t timer_args = {
        .callback = pair_timer_callback,
        .name = "pair_window",
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &s_pair_timer));
    esp_zb_platform_config_t config = {
        .radio_config = {.radio_mode = ZB_RADIO_MODE_NATIVE},
        .host_config = {.host_connection_mode = ZB_HOST_CONNECTION_MODE_NONE},
    };
    ESP_ERROR_CHECK(esp_zb_platform_config(&config));
    xTaskCreate(zigbee_task, "zigbee_hub", 4096, NULL, 5, NULL);
}
