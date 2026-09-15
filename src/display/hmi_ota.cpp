#include "hmi_ota.h"
#include "display_config.h"
#include "display_link.h"
#include "display_state.h"
#include <Arduino.h>
#include <HTTPClient.h>
#include <Network.h>
#include <Update.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <cstdarg>
#include <cstring>
#include <lwip/netdb.h>

namespace {

QueueHandle_t s_queue;
NetworkClientSecure s_tls;

/* The operator usually watches the gateway console, where only this 62-byte
 * status message arrives. Carry the failing stage in it so the cause is
 * visible without attaching a second USB cable to the HMI. */
char s_detail[24];

void setDetail(const char *format, ...) {
    va_list args;
    va_start(args, format);
    vsnprintf(s_detail, sizeof(s_detail), format, args);
    va_end(args);
}

void report(uint8_t stage, uint8_t result, const char *message) {
    display_hmi_ota_status_t status{};
    status.stage = stage;
    status.result = result;
    strncpy(status.message, message ? message : "", sizeof(status.message) - 1);
    displayStateSetOtaStatus(status);
    displaySendCommand(DISPLAY_MSG_HMI_OTA_STATUS, &status, sizeof(status));
    Serial.printf("[HMI OTA] stage=%u result=%u %s\n",
                  stage, result, status.message);
}

bool networkReady(void) {
    return WiFi.status() == WL_CONNECTED && WiFi.localIP()[0] != 0;
}

bool waitForNetwork(uint32_t timeout_ms) {
    const uint32_t started = millis();
    while (!networkReady() && millis() - started < timeout_ms) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    return networkReady();
}

void logNetwork(void) {
    Serial.printf(
        "[HMI OTA] ip=%s gw=%s dns=%s rssi=%d heap=%u internal=%u largest=%u\n",
        WiFi.localIP().toString().c_str(),
        WiFi.gatewayIP().toString().c_str(),
        WiFi.dnsIP().toString().c_str(), WiFi.RSSI(),
        (unsigned)ESP.getFreeHeap(),
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
}

void logTlsError(void) {
    char text[96];
    const int err = s_tls.lastError(text, sizeof(text));
    Serial.printf("[HMI OTA] tls lastError=%d %s\n", err, text);
    setDetail("tls%d", err);
}

/* Arduino 3.x enables modem sleep by default. The gateway never notices
 * because MQTT keeps the radio busy; the HMI brings Wi-Fi up only for OTA and
 * then starts a TLS handshake that the sleeping radio drops as connect -1. */
void holdRadioAwake(void) {
    WiFi.setSleep(WIFI_PS_NONE);
    if (WiFi.dnsIP()[0] == 0) {
        WiFi.config(WiFi.localIP(), WiFi.gatewayIP(), WiFi.subnetMask(),
                    IPAddress(8, 8, 8, 8), IPAddress(1, 1, 1, 1));
    }
}

bool parseHttpsUrl(const char *url, String &host, String &path) {
    if (strncmp(url, "https://", 8) != 0) return false;
    const char *start = url + 8;
    const char *slash = strchr(start, '/');
    if (!slash || slash == start) return false;
    host = String(start, (unsigned)(slash - start));
    path = slash;
    return host.length() > 0 && path.length() > 0;
}

/* GitHub publishes AAAA records. Arduino 3.x hostByName prefers IPv6 whenever
 * a global v6 address exists, and a TLS connect to that address fails on a
 * v4-only LAN with the same HTTPClient -1 the user is seeing. */
bool resolveIpv4(const char *host, IPAddress &ip) {
    struct addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo *result = nullptr;
    const int err = getaddrinfo(host, "443", &hints, &result);
    if (err != 0 || !result) {
        Serial.printf("[HMI OTA] IPv4 DNS failed host=%s err=%d\n", host, err);
        setDetail("dns%d", err);
        return false;
    }
    const auto *addr = reinterpret_cast<sockaddr_in *>(result->ai_addr);
    ip = IPAddress(addr->sin_addr.s_addr);
    freeaddrinfo(result);
    Serial.printf("[HMI OTA] DNS %s -> %s\n", host, ip.toString().c_str());
    return ip[0] != 0;
}

void configureHttp(HTTPClient &http, uint16_t timeout_ms) {
    http.setConnectTimeout(timeout_ms);
    http.setTimeout(timeout_ms);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    http.setReuse(false);
}

bool connectWifi(const display_wifi_credentials_t &credentials) {
    if (WiFi.status() == WL_CONNECTED &&
        WiFi.SSID() == String(credentials.ssid) && waitForNetwork(10000)) {
        holdRadioAwake();
        return true;
    }
    report(1, 0, "Connecting HMI to Wi-Fi...");
    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);
    holdRadioAwake();
    WiFi.begin(credentials.ssid, credentials.password);
    if (!waitForNetwork(20000)) {
        Serial.printf("[HMI OTA] Wi-Fi failed status=%d ssid=%s\n",
                      (int)WiFi.status(), credentials.ssid);
        report(1, 2, "HMI Wi-Fi connection failed");
        WiFi.disconnect(true, false);
        return false;
    }
    holdRadioAwake();
    /* Let DHCP/DNS and the radio leave power-save before the first TLS. */
    vTaskDelay(pdMS_TO_TICKS(750));
    logNetwork();
    report(1, 1, "HMI connected to Wi-Fi");
    return true;
}

int httpsGet(const char *url, HTTPClient &http) {
    String host;
    String path;
    if (!parseHttpsUrl(url, host, path)) {
        Serial.printf("[HMI OTA] bad URL %s\n", url);
        return 0;
    }
    /* Two 16 KiB mbedTLS I/O buffers plus contexts must come from internal
     * SRAM. Below this the handshake returns MBEDTLS_ERR_SSL_ALLOC_FAILED
     * (-0x7F00), so say so plainly instead of reporting a TLS error. */
    const size_t internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    if (internal_free < 45u * 1024u) {
        Serial.printf("[HMI OTA] internal RAM too low: %u bytes\n",
                      (unsigned)internal_free);
        setDetail("ram-low");
        return HTTPC_ERROR_CONNECTION_REFUSED;
    }

    IPAddress ip;
    setDetail("dns-start");
    if (!resolveIpv4(host.c_str(), ip)) {
        return HTTPC_ERROR_CONNECTION_REFUSED;
    }
    setDetail("tls-start");

    s_tls.stop();
    s_tls.setInsecure();
    s_tls.setHandshakeTimeout(30);
    if (!s_tls.connect(ip, 443, host.c_str(), nullptr, nullptr, nullptr)) {
        logTlsError();
        s_tls.stop();
        return HTTPC_ERROR_CONNECTION_REFUSED;
    }

    setDetail("http-start");
    configureHttp(http, 20000);
    if (!http.begin(s_tls, host, 443, path, true)) {
        setDetail("http-begin");
        s_tls.stop();
        return 0;
    }
    const int code = http.GET();
    setDetail("http%d", code);
    return code;
}

bool readAvailableVersion(String &version, int &code) {
    constexpr uint8_t ATTEMPTS = 3;
    code = 0;
    for (uint8_t attempt = 1; attempt <= ATTEMPTS; ++attempt) {
        HTTPClient http;
        code = httpsGet(HMI_VERSION_URL, http);
        if (code == HTTP_CODE_OK) {
            version = http.getString();
            version.trim();
        } else {
            Serial.printf("[HMI OTA] version GET %s\n",
                          http.errorToString(code).c_str());
        }
        http.end();
        s_tls.stop();
        if (code == HTTP_CODE_OK && version.length() > 0) return true;
        Serial.printf("[HMI OTA] version attempt %u/%u code=%d bytes=%u\n",
                      attempt, ATTEMPTS, code, (unsigned)version.length());
        logNetwork();
        if (attempt < ATTEMPTS) vTaskDelay(pdMS_TO_TICKS(1500));
    }
    return false;
}

bool installFirmware(void) {
    HTTPClient http;
    const int code = httpsGet(HMI_FIRMWARE_URL, http);
    if (code != HTTP_CODE_OK) {
        char message[62];
        snprintf(message, sizeof(message), "HMI firmware HTTP error %d", code);
        report(3, 2, message);
        http.end();
        s_tls.stop();
        return false;
    }
    const int contentLength = http.getSize();
    if (!Update.begin(contentLength > 0 ? (size_t)contentLength
                                       : UPDATE_SIZE_UNKNOWN)) {
        report(3, 2, "HMI OTA partition unavailable");
        http.end();
        s_tls.stop();
        return false;
    }
    report(3, 0, "Installing HMI update...");
    const size_t written = Update.writeStream(http.getStream());
    const bool complete = Update.end(true);
    http.end();
    s_tls.stop();
    if (!complete || (contentLength > 0 && written != (size_t)contentLength)) {
        report(3, 2, "HMI update write failed");
        return false;
    }
    report(3, 1, "HMI updated; restarting...");
    vTaskDelay(pdMS_TO_TICKS(750));
    ESP.restart();
    return true;
}

void otaTask(void *) {
    display_wifi_credentials_t credentials;
    for (;;) {
        if (xQueueReceive(s_queue, &credentials, portMAX_DELAY) != pdTRUE) continue;
        const uint8_t action = credentials.action;
        Serial.printf("[HMI OTA] request action=%u client=%s url=%s\n",
                      action, HMI_OTA_CLIENT_ID, HMI_VERSION_URL);
        if (!connectWifi(credentials)) {
            memset(&credentials, 0, sizeof(credentials));
            continue;
        }
        memset(credentials.password, 0, sizeof(credentials.password));
        String available;
        int code = 0;
        if (!readAvailableVersion(available, code)) {
            char message[sizeof(display_hmi_ota_status_t::message)];
            snprintf(message, sizeof(message),
                     "HMI check failed at %s ram=%uk", s_detail,
                     (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
            report(2, 2, message);
            memset(&credentials, 0, sizeof(credentials));
            continue;
        }
        if (available == HMI_FIRMWARE_VERSION) {
            report(2, 1, "HMI firmware is up to date");
        } else if (!action) {
            char message[62];
            snprintf(message, sizeof(message), "HMI update available: %s",
                     available.c_str());
            report(2, 1, message);
        } else {
            installFirmware();
        }
        memset(&credentials, 0, sizeof(credentials));
    }
}

}  // namespace

void hmiOtaBegin(void) {
    if (s_queue) return;
    s_queue = xQueueCreate(1, sizeof(display_wifi_credentials_t));
    if (s_queue) {
        /* mbedtls handshake plus HTTPClient need more than the previous 10 KiB. */
        xTaskCreatePinnedToCore(otaTask, "hmiOta", 24576, nullptr, 1, nullptr, 0);
    }
}

bool hmiOtaSubmit(const display_wifi_credentials_t &credentials) {
    return s_queue && xQueueOverwrite(s_queue, &credentials) == pdTRUE;
}
