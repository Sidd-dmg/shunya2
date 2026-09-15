#include "wifi_prov.h"
#include "app_state.h"
#include "config.h"
#include <WiFi.h>
#include <WiFiProv.h>
#include <Arduino.h>

static void SysProvEvent(arduino_event_t *sys_event) {
    switch (sys_event->event_id) {
        case ARDUINO_EVENT_WIFI_STA_GOT_IP:
            Serial.print("\nConnected to WiFi! IP Address: ");
            Serial.println(IPAddress(sys_event->event_info.got_ip.ip_info.ip.addr));
            wifi_flag = 2;
            control_states[6] = 0;
            break;
        case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
            Serial.println("\nDisconnected from WiFi");
            wifi_flag = 1;
            break;
        case ARDUINO_EVENT_PROV_START:
            wifi_flag = 0;
            control_states[6] = 1;
            Serial.println("\nProvisioning started");
            break;
        case ARDUINO_EVENT_PROV_CRED_RECV:
            Serial.println("\nReceived WiFi credentials");
            break;
        case ARDUINO_EVENT_PROV_CRED_SUCCESS:
            Serial.println("\nProvisioning successful");
            break;
        case ARDUINO_EVENT_PROV_END:
            Serial.println("\nProvisioning ended");
            break;
        default:
            break;
    }
}

static void startBleProvisioning(void) {
    Serial.println("Starting BLE provisioning...");
    WiFi.onEvent(SysProvEvent);
    WiFiProv.beginProvision(
        WIFI_PROV_SCHEME_BLE,
        WIFI_PROV_SCHEME_HANDLER_FREE_BTDM,
        WIFI_PROV_SECURITY_1,
        PROV_POP_PIN,
        provServiceName.c_str());
    Serial.println("Service Name: " + provServiceName);
    Serial.println("POP: " + String(PROV_POP_PIN));
}

void setupWiFi(void) {
    if (WiFi.begin() == WL_CONNECT_FAILED) {
        startBleProvisioning();
        return;
    }
    Serial.print("Connecting to saved WiFi");
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 20) {
        delay(500);
        Serial.print(".");
        attempts++;
    }
    if (WiFi.status() == WL_CONNECTED) {
        Serial.println("\nWiFi connected");
        wifi_flag = 2;
        Serial.print("IP address: ");
        Serial.println(WiFi.localIP());
    } else {
        Serial.println("\nWiFi connection failed.");
        startBleProvisioning();
    }
}

void resetWiFiCredentials(void) {
    Serial.println("\n=== Resetting WiFi Credentials ===");
    WiFi.disconnect(true, true);
    delay(1000);
    ESP.restart();
}
