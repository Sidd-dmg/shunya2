#ifndef SHUNYA_THREAD_HUB_H
#define SHUNYA_THREAD_HUB_H

#include <stdbool.h>
#include <stdint.h>

void thread_hub_start(void);
void thread_hub_pair_open(uint16_t duration_sec);
void thread_hub_pair_close(void);
void thread_hub_forget_node(uint8_t node_id, const uint8_t *eui64);
void thread_hub_forget_all(void);
bool thread_hub_pair_is_open(void);
uint16_t thread_hub_pair_remaining(void);
uint16_t thread_hub_pan_id(void);
uint8_t thread_hub_channel(void);

#endif
