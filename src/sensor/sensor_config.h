#ifndef SENSOR_CONFIG_H
#define SENSOR_CONFIG_H

#include "hal/adc_types.h"

/* 11 = DHT11, 22 = DHT22 / AM2302 */
#define DHT_MODEL           22
#define DHT_GPIO            6
#define DHT_POWER_GPIO      -1

#define BUTTON_GPIO         9
#define BUTTON_RESET_MS     3000

/* Optional battery sense. GPIO 2 / ADC1_CH2. Set GPIO to -1 to disable. */
#define BATTERY_ADC_GPIO      2
#define BATTERY_ADC_CHANNEL   ADC_CHANNEL_2
#define BATTERY_ADC_SCALE     2
#define BATTERY_EMPTY_MV      3000
#define BATTERY_FULL_MV       4200

#ifdef SENSOR_DEMO_SERIAL
#define REPORT_INTERVAL_MS  5000
#define SENSOR_MODEL        "DEMO-SERIAL-H2"
#else
#define REPORT_INTERVAL_MS  20000
#define SENSOR_MODEL        "DHT-ZIGBEE"
#endif

#define SENSOR_MANUFACTURER "Shunya"

#endif
