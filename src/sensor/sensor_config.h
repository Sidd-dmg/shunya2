#ifndef SENSOR_CONFIG_H
#define SENSOR_CONFIG_H

#include "hal/adc_types.h"

/* ESP32-C6-MINI-1 (DevKitM-1). Leave GPIO 8 for the kit RGB LED and GPIO 12/13
 * for USB D-/D+. UART0, if an external adapter is used, is GPIO16 TX / GPIO17 RX. */

/* Sensirion SHT40. SCL is GPIO 23, SDA is GPIO 22. Address 0x44. */
#define SHT40_SDA_GPIO      22
#define SHT40_SCL_GPIO      23
#define SHT40_I2C_ADDR      0x44

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
#define SENSOR_MODEL        "SHT40"
#endif

#define SENSOR_MANUFACTURER "Shunya"

#endif
