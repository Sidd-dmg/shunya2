#ifndef SHUNYA_ZB_H
#define SHUNYA_ZB_H

#include <stdint.h>

#define SHUNYA_ZB_ENDPOINT           1
#define SHUNYA_ZB_CLUSTER_ID         0xFC00u
#define SHUNYA_ZB_ATTR_SLEEP_SEC     0x0000u
#define SHUNYA_ZB_ATTR_BATTERY_PCT   0x0001u
#define SHUNYA_ZB_MANUFACTURER_CODE  0x131Bu

#define SENSOR_SLEEP_DEFAULT_S  20u
#define SENSOR_SLEEP_MIN_S      5u
#define SENSOR_SLEEP_MAX_S      3600u

#define BATTERY_PCT_UNKNOWN     0xFFu

static inline uint16_t shunya_clamp_sleep_sec(uint16_t seconds)
{
    if (seconds < SENSOR_SLEEP_MIN_S) {
        return SENSOR_SLEEP_MIN_S;
    }
    if (seconds > SENSOR_SLEEP_MAX_S) {
        return SENSOR_SLEEP_MAX_S;
    }
    return seconds;
}

#endif
