#ifndef GATEWAY_MQTT_SERVICE_H
#define GATEWAY_MQTT_SERVICE_H

#include <Arduino.h>
#include <stdint.h>

void mqttServiceBegin(void);
bool mqttIsConnected(void);
void mqttEnqueuePublish(const char *topic, const char *payload, bool retain);
void mqttTask(void *pvParameters);
void publishStatus(void);
void publishSensorData(uint8_t nodeId, float temperature, float humidity, uint8_t batteryPercent);
void processPendingNtp(void);
String getCurrentTimestamp(void);

#endif
