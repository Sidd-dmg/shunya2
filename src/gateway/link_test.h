#ifndef GATEWAY_LINK_TEST_H
#define GATEWAY_LINK_TEST_H

#include <stdint.h>

#define LINK_TEST_HUB           1
#define LINK_TEST_DISPLAY_TX    2
#define LINK_TEST_DISPLAY_ECHO  3
#define LINK_TEST_DISPLAY_LOOPBACK 4
#define LINK_TEST_ALL           5
#define LINK_TEST_STATS         6

void linkTestBegin(void);
void linkTestRequest(uint8_t kind);
void linkTestTask(void *pvParameters);

#endif
