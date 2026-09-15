#ifndef HUB_UART_H
#define HUB_UART_H

#include <stdint.h>
#include <stddef.h>
#include "slots.h"

void hub_uart_init(void);
void hub_uart_send(uint8_t cmd, const uint8_t *payload, uint8_t length);
void hub_uart_task(void *arg);
void hub_uart_send_sensor(const hub_slot_t *s);
void hub_uart_send_joined(const hub_slot_t *s);
void hub_uart_send_left(const hub_slot_t *s);

#endif
