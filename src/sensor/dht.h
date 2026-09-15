#ifndef SENSOR_DHT_H
#define SENSOR_DHT_H

#include "esp_err.h"

esp_err_t dht_init(int gpio);
esp_err_t dht_read(int gpio, int model, float *temperature_c, float *humidity_pct);

#endif
