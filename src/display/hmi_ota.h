#ifndef SHUNYA_HMI_OTA_H
#define SHUNYA_HMI_OTA_H

#include "display_protocol.h"

/* Bumped whenever the OTA transport changes, so a serial log proves which
 * client is running instead of relying on the firmware version alone. */
#define HMI_OTA_CLIENT_ID "ipv4-only/ps-none/static-tls"

void hmiOtaBegin(void);
bool hmiOtaSubmit(const display_wifi_credentials_t &credentials);

#endif
