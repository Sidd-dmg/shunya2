#include "slots.h"
#include "hub_config.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "slots";
static hub_slot_t s_slots[HUB_MAX_NODES];

void slots_init(void) {
    memset(s_slots, 0, sizeof(s_slots));
    nvs_handle_t h;
    if (nvs_open(HUB_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    size_t len = sizeof(s_slots);
    nvs_get_blob(h, HUB_NVS_SLOTS_KEY, s_slots, &len);
    nvs_close(h);
}

void slots_save(void) {
    nvs_handle_t h;
    if (nvs_open(HUB_NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGE(TAG, "nvs open failed");
        return;
    }
    nvs_set_blob(h, HUB_NVS_SLOTS_KEY, s_slots, sizeof(s_slots));
    nvs_commit(h);
    nvs_close(h);
}

hub_slot_t *slots_table(void) {
    return s_slots;
}

int slots_count(void) {
    int n = 0;
    for (int i = 0; i < HUB_MAX_NODES; i++) {
        if (s_slots[i].used) {
            n++;
        }
    }
    return n;
}

int slots_find_ieee(const uint8_t *ieee) {
    for (int i = 0; i < HUB_MAX_NODES; i++) {
        if (s_slots[i].used && memcmp(s_slots[i].ieee, ieee, 8) == 0) {
            return i;
        }
    }
    return -1;
}

int slots_find_short(uint16_t short_addr) {
    for (int i = 0; i < HUB_MAX_NODES; i++) {
        if (s_slots[i].used && s_slots[i].short_addr == short_addr) {
            return i;
        }
    }
    return -1;
}

int slots_alloc(const uint8_t *ieee, uint16_t short_addr) {
    int existing = slots_find_ieee(ieee);
    if (existing >= 0) {
        s_slots[existing].short_addr = short_addr;
        slots_save();
        return existing;
    }
    for (int i = 0; i < HUB_MAX_NODES; i++) {
        if (!s_slots[i].used) {
            s_slots[i].used = 1;
            s_slots[i].node_id = (uint8_t)(i + 1);
            memcpy(s_slots[i].ieee, ieee, 8);
            s_slots[i].short_addr = short_addr;
            s_slots[i].lqi = 0;
            s_slots[i].battery_pct = 0xFF;
            s_slots[i].last_temp = 0;
            s_slots[i].last_hum = 0;
            slots_save();
            return i;
        }
    }
    return -1;
}

void slots_free_index(int idx) {
    if (idx < 0 || idx >= HUB_MAX_NODES) {
        return;
    }
    memset(&s_slots[idx], 0, sizeof(s_slots[idx]));
    slots_save();
}

void slots_clear_all(void) {
    memset(s_slots, 0, sizeof(s_slots));
    slots_save();
}

void slots_update_sensor(int idx, float temp, float hum, uint8_t lqi, uint8_t battery_pct) {
    if (idx < 0 || idx >= HUB_MAX_NODES || !s_slots[idx].used) {
        return;
    }
    s_slots[idx].last_temp = temp;
    s_slots[idx].last_hum = hum;
    s_slots[idx].lqi = lqi;
    s_slots[idx].battery_pct = battery_pct;
}
