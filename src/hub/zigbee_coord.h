#ifndef SHUNYA_ZIGBEE_COORD_H
#define SHUNYA_ZIGBEE_COORD_H

#include <stdbool.h>
#include <stdint.h>

void zigbee_coord_start(void);
void zigbee_coord_pair_open(uint16_t duration_sec);
void zigbee_coord_pair_close(void);
void zigbee_coord_forget_node(uint8_t node_id, const uint8_t *ieee);
void zigbee_coord_forget_all(void);
void zigbee_coord_set_sleep_sec(uint16_t seconds);
bool zigbee_coord_pair_is_open(void);
uint16_t zigbee_coord_pair_remaining(void);
uint16_t zigbee_coord_pan_id(void);
uint8_t zigbee_coord_channel(void);
uint16_t zigbee_coord_sleep_sec(void);

#endif
