#ifndef HUB_SLOTS_H
#define HUB_SLOTS_H

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    uint8_t used;
    uint8_t node_id;
    uint8_t ieee[8];
    uint16_t short_addr;
    uint8_t lqi;
    uint8_t battery_pct;
    float last_temp;
    float last_hum;
} hub_slot_t;

void slots_init(void);
void slots_save(void);
hub_slot_t *slots_table(void);
int slots_count(void);
int slots_find_ieee(const uint8_t *ieee);
int slots_find_short(uint16_t short_addr);
int slots_alloc(const uint8_t *ieee, uint16_t short_addr);
void slots_free_index(int idx);
void slots_clear_all(void);
void slots_update_sensor(int idx, float temp, float hum, uint8_t lqi, uint8_t battery_pct);

#endif
