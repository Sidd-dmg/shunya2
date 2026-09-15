#ifndef SHUNYA_DISPLAY_CLIENT_LINK_H
#define SHUNYA_DISPLAY_CLIENT_LINK_H

#include "display_protocol.h"
#include <stdint.h>

void displayLinkBegin();
bool displayLinkConnected();
uint16_t displaySendCommand(uint8_t type, const void *payload = nullptr, uint16_t length = 0);
void displayRequestSnapshot();
void displayLinkTask(void *arg);

#endif
