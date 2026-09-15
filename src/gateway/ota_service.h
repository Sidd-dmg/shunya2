#ifndef GATEWAY_OTA_SERVICE_H
#define GATEWAY_OTA_SERVICE_H

void otaTask(void *pvParameters);
void markFirmwareValid(void);
void requestOtaUpdate(void);
void requestOtaCheck(void);

#endif
