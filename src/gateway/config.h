#ifndef GATEWAY_CONFIG_H
#define GATEWAY_CONFIG_H

#include <stdint.h>

#include "shunya_zb.h"

#define CURRENT_VERSION "4.2.1"

#define MQTT_SERVER   "82.112.230.108"
#define MQTT_PORT     1883
#define MQTT_USER     "shunya"
#define MQTT_PASSWORD "shunyaIOT"

#define NTP_SERVER            "pool.ntp.org"
#define GMT_OFFSET_SEC        19800
#define DAYLIGHT_OFFSET_SEC   0
#define NTP_SYNC_INTERVAL_MS  3600000UL

#define OTA_URL         "https://raw.githubusercontent.com/Sidd-dmg/yoobee-shunya/refs/heads/main/ota/32e/firmware.bin"
#define VERSION_URL     "https://raw.githubusercontent.com/Sidd-dmg/yoobee-shunya/refs/heads/main/ota/32e/version.txt"
#define OTA_CHECK_INTERVAL_MS 3600000UL

#define BOOT_BUTTON_PIN  0
#define RELAY_COUNT      7
#define RELAY_MASK_ALL   ((uint8_t)((1u << RELAY_COUNT) - 1u))
#define RELAY1_PIN       15
#define RELAY2_PIN       16
#define RELAY3_PIN       9
#define RELAY4_PIN       14
#define RELAY5_PIN       21
#define RELAY6_PIN       47
#define RELAY7_PIN       48

#define DISPLAY_UART_RX     44
#define DISPLAY_UART_TX     43
#define DISPLAY_UART_BAUD   115200
#define DisplaySerial       Serial2

/* S3 Serial1 <-> C6 hub UART1. Cross TX/RX on the PCB. */
#define HUB_UART_RX      17
#define HUB_UART_TX      18
#define HUB_UART_BAUD    115200

#define MAX_NODES        16
#define MAX_AUTO_RULES   16
#define MAX_QUEUE_STEPS  20

/* The used map ends at address 832. A 4 KB image cannot be replaced inside
 * the 20 KB NVS partition, so commits fail with ESP_ERR_NVS_NOT_ENOUGH_SPACE. */
#define EEPROM_SIZE               1024
/* Legacy v1 node storage is retained solely for one-time migration. */
#define EEPROM_NODE_COUNT_ADDR    0
#define EEPROM_NODES_START_ADDR   1
#define EEPROM_NODE_RECORD_SIZE   11
#define EEPROM_MODE_ADDR          100
#define EEPROM_MANUAL_RELAYS_ADDR 101 /* RELAY_COUNT bytes; automation rules start at 110 */
#define EEPROM_AUTO_RULES_ADDR    110
#define EEPROM_QUEUE_STATE_ADDR   290
#define EEPROM_QUEUE_CONFIG_ADDR  300
#define EEPROM_HYBRID_CONFIG_ADDR 450
#define EEPROM_SENSOR_SLEEP_ADDR  496
/* Versioned node storage must not overlap the legacy configuration map. */
#define EEPROM_NODES_SCHEMA_ADDR      500
#define EEPROM_NODES_V2_START_ADDR    512
#define EEPROM_NODES_SCHEMA_MAGIC     0x534E4F44UL
#define EEPROM_NODES_SCHEMA_VERSION   3
/* v3 adds temperature, humidity, and battery after the 11-byte identity. */
#define EEPROM_NODE_RECORD_V3_SIZE    20

#define PAIRING_MODE_DURATION_MS 120000UL
#define BUTTON_HOLD_TIME_MS      1000UL
#define RESET_WIFI_TIME_MS       5000UL
#define RESET_NODES_TIME_MS      10000UL
#define FACTORY_RESET_TIME_MS    20000UL

#define MODE_MANUAL     0
#define MODE_AUTOMATION 1
#define MODE_QUEUE      2
#define MODE_HYBRID     3

#define EVENT_LOG_FILE        "/event_log.jsonl"
#define EVENT_LOG_MAX_ENTRIES 5000
#define EVENT_LOG_BATCH_SIZE  20

#define STATUS_PUBLISH_INTERVAL_MS 10000UL
#define PROV_POP_PIN "abcd1234"

#define MQTT_CMD_PAYLOAD_MAX 3072
#define MQTT_PUB_PAYLOAD_MAX 3072

#endif
