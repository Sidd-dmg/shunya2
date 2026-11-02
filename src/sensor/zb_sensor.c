#include "zb_sensor.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "driver/gpio.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ha/esp_zigbee_ha_standard.h"
#include "nvs.h"
#include "sensor_config.h"
#include "shunya_zb.h"
#ifndef SENSOR_DEMO_SERIAL
#include "sht40.h"
#endif

static const char *TAG = "zb_sensor";
static TaskHandle_t s_report_task;
static volatile bool s_joined;
static uint16_t s_sleep_sec = SENSOR_SLEEP_DEFAULT_S;
static uint8_t s_battery_pct = BATTERY_PCT_UNKNOWN;
#ifdef SENSOR_DEMO_SERIAL
static portMUX_TYPE s_demo_lock = portMUX_INITIALIZER_UNLOCKED;
static int16_t s_demo_temp_x100;
static uint16_t s_demo_hum_x100;
static uint8_t s_demo_battery = BATTERY_PCT_UNKNOWN;
static bool s_demo_has_value;
#endif
#if BATTERY_ADC_GPIO >= 0
static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_adc_cali;
static bool s_adc_calibrated;
#endif

static void persist_sleep(uint16_t seconds)
{
    nvs_handle_t nvs;
    if (nvs_open("zbsensor", NVS_READWRITE, &nvs) != ESP_OK) {
        return;
    }
    nvs_set_u16(nvs, "sleep", seconds);
    nvs_commit(nvs);
    nvs_close(nvs);
}

static void load_sleep(void)
{
    nvs_handle_t nvs;
    if (nvs_open("zbsensor", NVS_READONLY, &nvs) != ESP_OK) {
        return;
    }
    uint16_t stored = SENSOR_SLEEP_DEFAULT_S;
    if (nvs_get_u16(nvs, "sleep", &stored) == ESP_OK) {
        s_sleep_sec = shunya_clamp_sleep_sec(stored);
    }
    nvs_close(nvs);
}

#if BATTERY_ADC_GPIO >= 0
static void battery_init(void)
{
    adc_oneshot_unit_init_cfg_t unit_cfg = {.unit_id = ADC_UNIT_1};
    if (adc_oneshot_new_unit(&unit_cfg, &s_adc) != ESP_OK) {
        return;
    }
    adc_oneshot_chan_cfg_t chan_cfg = {
        .bitwidth = ADC_BITWIDTH_DEFAULT,
        .atten = ADC_ATTEN_DB_12,
    };
    adc_oneshot_config_channel(s_adc, BATTERY_ADC_CHANNEL, &chan_cfg);
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t cali = {
        .unit_id = ADC_UNIT_1,
        .chan = BATTERY_ADC_CHANNEL,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    s_adc_calibrated = adc_cali_create_scheme_curve_fitting(&cali, &s_adc_cali) == ESP_OK;
#endif
}

static uint8_t battery_read_percent(void)
{
    if (!s_adc) {
        return BATTERY_PCT_UNKNOWN;
    }
    int raw = 0;
    if (adc_oneshot_read(s_adc, BATTERY_ADC_CHANNEL, &raw) != ESP_OK) {
        return BATTERY_PCT_UNKNOWN;
    }
    int mv = 0;
    if (s_adc_calibrated) {
        adc_cali_raw_to_voltage(s_adc_cali, raw, &mv);
    } else {
        mv = raw; /* uncalibrated fallback; still maps to a percentage band */
    }
    mv *= BATTERY_ADC_SCALE;
    if (mv <= BATTERY_EMPTY_MV) {
        return 0;
    }
    if (mv >= BATTERY_FULL_MV) {
        return 100;
    }
    return (uint8_t)(((mv - BATTERY_EMPTY_MV) * 100) / (BATTERY_FULL_MV - BATTERY_EMPTY_MV));
}
#endif

static void set_attr_u16(uint16_t cluster, uint16_t attr, uint16_t value)
{
    esp_zb_zcl_set_attribute_val(SHUNYA_ZB_ENDPOINT, cluster, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
                                 attr, &value, false);
}

static void set_attr_u8(uint16_t cluster, uint16_t attr, uint8_t value)
{
    esp_zb_zcl_set_attribute_val(SHUNYA_ZB_ENDPOINT, cluster, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
                                 attr, &value, false);
}

static void report_attr(uint16_t cluster, uint16_t attr)
{
    /* Bound-only delivery never leaves this device: the coordinator writes
     * sleep by short address, but nothing creates a binding for reports. */
    esp_zb_zcl_report_attr_cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.zcl_basic_cmd.dst_addr_u.addr_short = 0x0000;
    cmd.zcl_basic_cmd.dst_endpoint = SHUNYA_ZB_ENDPOINT;
    cmd.zcl_basic_cmd.src_endpoint = SHUNYA_ZB_ENDPOINT;
    cmd.address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;
    cmd.clusterID = cluster;
    cmd.attributeID = attr;
    cmd.direction = ESP_ZB_ZCL_CMD_DIRECTION_TO_CLI;
    esp_err_t err = esp_zb_zcl_report_attr_cmd_req(&cmd);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "report cluster 0x%04x attr 0x%04x failed: %s",
                 cluster, attr, esp_err_to_name(err));
    }
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
        esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_INITIALIZATION);
        break;
    case ESP_ZB_BDB_SIGNAL_DEVICE_FIRST_START:
    case ESP_ZB_BDB_SIGNAL_DEVICE_REBOOT:
        if (err_status == ESP_OK) {
            if (esp_zb_bdb_is_factory_new()) {
                ESP_LOGI(TAG, "Start network steering");
                esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_NETWORK_STEERING);
            } else {
                s_joined = true;
                if (s_report_task) {
                    xTaskNotifyGive(s_report_task);
                }
                ESP_LOGI(TAG, "Joined network PAN 0x%04X ch %d",
                         esp_zb_get_pan_id(), esp_zb_get_current_channel());
            }
        } else {
            ESP_LOGW(TAG, "Zigbee init failed: %s", esp_err_to_name(err_status));
            esp_zb_scheduler_alarm((esp_zb_callback_t)bdb_start_top_level_commissioning_cb,
                                   ESP_ZB_BDB_MODE_NETWORK_STEERING, 2000);
        }
        break;
    case ESP_ZB_BDB_SIGNAL_STEERING:
        if (err_status == ESP_OK) {
            s_joined = true;
            ESP_LOGI(TAG, "Joined network PAN 0x%04X ch %d short 0x%04X",
                     esp_zb_get_pan_id(), esp_zb_get_current_channel(),
                     esp_zb_get_short_address());
            if (s_report_task) {
                xTaskNotifyGive(s_report_task);
            }
        } else {
            ESP_LOGI(TAG, "Join failed (%s)", esp_err_to_name(err_status));
            s_joined = false;
            esp_zb_scheduler_alarm((esp_zb_callback_t)bdb_start_top_level_commissioning_cb,
                                   ESP_ZB_BDB_MODE_NETWORK_STEERING, 2000);
        }
        break;
    default:
        break;
    }
}

static esp_err_t zb_attribute_handler(const esp_zb_zcl_set_attr_value_message_t *message)
{
    if (!message || message->info.status != ESP_ZB_ZCL_STATUS_SUCCESS) {
        return ESP_FAIL;
    }
    if (message->info.cluster == SHUNYA_ZB_CLUSTER_ID &&
        message->attribute.id == SHUNYA_ZB_ATTR_SLEEP_SEC &&
        message->attribute.data.value && message->attribute.data.size >= 2) {
        const uint8_t *bytes = message->attribute.data.value;
        uint16_t seconds = shunya_clamp_sleep_sec(
            (uint16_t)(bytes[0] | ((uint16_t)bytes[1] << 8)));
        if (seconds == s_sleep_sec) {
            return ESP_OK;
        }
        s_sleep_sec = seconds;
        persist_sleep(s_sleep_sec);
        ESP_LOGI(TAG, "sleep interval set to %u s", s_sleep_sec);
        if (s_report_task) {
            xTaskNotifyGive(s_report_task);
        }
    }
    return ESP_OK;
}

static esp_err_t zb_action_handler(esp_zb_core_action_callback_id_t callback_id, const void *message)
{
    if (callback_id == ESP_ZB_CORE_SET_ATTR_VALUE_CB_ID) {
        return zb_attribute_handler(message);
    }
    return ESP_OK;
}

#ifdef SENSOR_DEMO_SERIAL
static bool parse_demo_values(char *line, float *temperature, float *humidity, uint8_t *battery)
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
    cursor = end;
    while (isspace((unsigned char)*cursor)) ++cursor;
    uint8_t b = BATTERY_PCT_UNKNOWN;
    if (*cursor) {
        const float battery_in = strtof(cursor, &end);
        if (end == cursor || !isfinite(battery_in) || battery_in < 0 || battery_in > 100) {
            return false;
        }
        b = (uint8_t)lroundf(battery_in);
        cursor = end;
    }
    while (isspace((unsigned char)*end)) ++end;
    if (*end || !isfinite(t) || !isfinite(h) || t < -40 || t > 80 || h < 0 || h > 100) return false;
    *temperature = t;
    *humidity = h;
    *battery = b;
    return true;
}

static void demo_serial_task(void *arg)
{
    (void)arg;
    char line[96];
    size_t used = 0;
    /* stdin is fully buffered on this console, so a typed line would sit
     * unread until the buffer filled. */
    setvbuf(stdin, NULL, _IONBF, 0);
    ESP_LOGI(TAG, "Serial input ready. Type: set 24.5 55.0  [battery%%]");
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
            uint8_t b;
            if (!parse_demo_values(line, &t, &h, &b)) {
                ESP_LOGW(TAG, "Invalid input '%s'. Use: set <temp_C> <humidity_%%> [battery_%%]", line);
                continue;
            }
            taskENTER_CRITICAL(&s_demo_lock);
            s_demo_temp_x100 = (int16_t)lroundf(t * 100.0f);
            s_demo_hum_x100 = (uint16_t)lroundf(h * 100.0f);
            s_demo_battery = b;
            s_demo_has_value = true;
            taskEXIT_CRITICAL(&s_demo_lock);
            if (s_report_task) xTaskNotifyGive(s_report_task);
            ESP_LOGI(TAG, "Demo set T=%.2f H=%.2f battery=%u", t, h,
                     b == BATTERY_PCT_UNKNOWN ? 0 : b);
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
        if (!s_joined) {
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(2000));
            continue;
        }

        int16_t temp_x100 = 0;
        uint16_t hum_x100 = 0;
        bool have_value = false;
#ifdef SENSOR_DEMO_SERIAL
        taskENTER_CRITICAL(&s_demo_lock);
        temp_x100 = s_demo_temp_x100;
        hum_x100 = s_demo_hum_x100;
        if (s_demo_battery != BATTERY_PCT_UNKNOWN) {
            s_battery_pct = s_demo_battery;
        }
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
#if BATTERY_ADC_GPIO >= 0
        s_battery_pct = battery_read_percent();
#endif
#endif
        if (have_value) {
            esp_zb_lock_acquire(portMAX_DELAY);
            set_attr_u16(ESP_ZB_ZCL_CLUSTER_ID_TEMP_MEASUREMENT,
                         ESP_ZB_ZCL_ATTR_TEMP_MEASUREMENT_VALUE_ID, (uint16_t)temp_x100);
            set_attr_u16(ESP_ZB_ZCL_CLUSTER_ID_REL_HUMIDITY_MEASUREMENT,
                         ESP_ZB_ZCL_ATTR_REL_HUMIDITY_MEASUREMENT_VALUE_ID, hum_x100);
            set_attr_u8(SHUNYA_ZB_CLUSTER_ID, SHUNYA_ZB_ATTR_BATTERY_PCT, s_battery_pct);
            set_attr_u16(SHUNYA_ZB_CLUSTER_ID, SHUNYA_ZB_ATTR_SLEEP_SEC, s_sleep_sec);
            report_attr(ESP_ZB_ZCL_CLUSTER_ID_TEMP_MEASUREMENT,
                        ESP_ZB_ZCL_ATTR_TEMP_MEASUREMENT_VALUE_ID);
            report_attr(ESP_ZB_ZCL_CLUSTER_ID_REL_HUMIDITY_MEASUREMENT,
                        ESP_ZB_ZCL_ATTR_REL_HUMIDITY_MEASUREMENT_VALUE_ID);
            report_attr(SHUNYA_ZB_CLUSTER_ID, SHUNYA_ZB_ATTR_BATTERY_PCT);
            esp_zb_lock_release();
            ESP_LOGI(TAG, "SHT40 T=%.2f H=%.2f battery=%u%% sleep=%us",
                     temp_x100 / 100.0f, hum_x100 / 100.0f,
                     s_battery_pct == BATTERY_PCT_UNKNOWN ? 0 : s_battery_pct, s_sleep_sec);
        }
        /* Stay awake briefly so the hub can write a new sleep interval. */
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(800));
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS((uint32_t)s_sleep_sec * 1000u));
    }
}

static esp_zb_cluster_list_t *create_sensor_clusters(void)
{
    esp_zb_cluster_list_t *cluster_list = esp_zb_zcl_cluster_list_create();
    esp_zb_basic_cluster_cfg_t basic_cfg = {
        .zcl_version = ESP_ZB_ZCL_BASIC_ZCL_VERSION_DEFAULT_VALUE,
        .power_source = 0x03, /* battery */
    };
    esp_zb_attribute_list_t *basic = esp_zb_basic_cluster_create(&basic_cfg);
    char manufacturer[] = "\x06" "Shunya";
#ifdef SENSOR_DEMO_SERIAL
    char model[] = "\x0E" "DEMO-SERIAL-H2";
#else
    char model[] = "\x05" "SHT40";
#endif
    esp_zb_basic_cluster_add_attr(basic, ESP_ZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID, manufacturer);
    esp_zb_basic_cluster_add_attr(basic, ESP_ZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID, model);
    esp_zb_cluster_list_add_basic_cluster(cluster_list, basic, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    esp_zb_identify_cluster_cfg_t identify_cfg = {
        .identify_time = ESP_ZB_ZCL_IDENTIFY_IDENTIFY_TIME_DEFAULT_VALUE,
    };
    esp_zb_cluster_list_add_identify_cluster(cluster_list,
                                             esp_zb_identify_cluster_create(&identify_cfg),
                                             ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    esp_zb_temperature_meas_cluster_cfg_t temp_cfg = {
        .measured_value = (int16_t)0x8000,
        .min_value = -4000,
        .max_value = 8000,
    };
    esp_zb_cluster_list_add_temperature_meas_cluster(
        cluster_list, esp_zb_temperature_meas_cluster_create(&temp_cfg),
        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    esp_zb_humidity_meas_cluster_cfg_t hum_cfg = {
        .measured_value = 0xFFFF,
        .min_value = 0,
        .max_value = 10000,
    };
    esp_zb_cluster_list_add_humidity_meas_cluster(
        cluster_list, esp_zb_humidity_meas_cluster_create(&hum_cfg),
        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    uint16_t sleep = s_sleep_sec;
    uint8_t battery = s_battery_pct;
    esp_zb_attribute_list_t *custom = esp_zb_zcl_attr_list_create(SHUNYA_ZB_CLUSTER_ID);
    ESP_ERROR_CHECK(esp_zb_cluster_add_attr(custom, SHUNYA_ZB_CLUSTER_ID, SHUNYA_ZB_ATTR_SLEEP_SEC,
                                            ESP_ZB_ZCL_ATTR_TYPE_U16,
                                            ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE, &sleep));
    ESP_ERROR_CHECK(esp_zb_cluster_add_attr(custom, SHUNYA_ZB_CLUSTER_ID, SHUNYA_ZB_ATTR_BATTERY_PCT,
                                            ESP_ZB_ZCL_ATTR_TYPE_U8,
                                            ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY | ESP_ZB_ZCL_ATTR_ACCESS_REPORTING,
                                            &battery));
    ESP_ERROR_CHECK(esp_zb_cluster_list_add_custom_cluster(cluster_list, custom,
                                                           ESP_ZB_ZCL_CLUSTER_SERVER_ROLE));
    return cluster_list;
}

static void zigbee_task(void *arg)
{
    (void)arg;
    esp_zb_cfg_t zb_nwk_cfg = {
        .esp_zb_role = ESP_ZB_DEVICE_TYPE_ED,
        .install_code_policy = false,
        .nwk_cfg.zed_cfg = {
            .ed_timeout = ESP_ZB_ED_AGING_TIMEOUT_64MIN,
            .keep_alive = 3000,
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
    esp_zb_ep_list_add_ep(ep_list, create_sensor_clusters(), ep_config);
    esp_zb_device_register(ep_list);
    esp_zb_core_action_handler_register(zb_action_handler);
    esp_zb_set_primary_network_channel_set(ESP_ZB_TRANSCEIVER_ALL_CHANNELS_MASK);
    ESP_ERROR_CHECK(esp_zb_start(false));
    esp_zb_stack_main_loop();
}

void zb_sensor_start(void)
{
    load_sleep();
#if BATTERY_ADC_GPIO >= 0 && !defined(SENSOR_DEMO_SERIAL)
    battery_init();
#endif
    xTaskCreate(report_task, "sensor_report", 4096, NULL, 3, NULL);
#ifdef SENSOR_DEMO_SERIAL
    xTaskCreate(demo_serial_task, "demo_serial", 4096, NULL, 2, NULL);
#endif
    esp_zb_platform_config_t config = {
        .radio_config = {.radio_mode = ZB_RADIO_MODE_NATIVE},
        .host_config = {.host_connection_mode = ZB_HOST_CONNECTION_MODE_NONE},
    };
    ESP_ERROR_CHECK(esp_zb_platform_config(&config));
    xTaskCreate(zigbee_task, "zigbee_sensor", 4096, NULL, 5, NULL);
}

void zb_sensor_factory_reset(void)
{
    esp_zb_factory_reset();
}
