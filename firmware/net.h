// ================================================================
//  net.h — HTTPS POST helpers (fresh client per request)
//
//  ESP32 WiFiClientSecure leaves stale TLS state when reused
//  across requests — http.begin() fails with code -1.
//  Creating a fresh client each time is the only reliable approach.
//  Mutex guards concurrent access from alert + heartbeat tasks.
// ================================================================

#pragma once
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <freertos/semphr.h>

#ifndef API_BASE_URL
#define API_BASE_URL ""
#endif
#ifndef API_KEY
#define API_KEY ""
#endif

static SemaphoreHandle_t g_netMutex = nullptr;

inline void netInit() {
    g_netMutex = xSemaphoreCreateMutex();
}

static int _httpsPost(const char* path, const String& body) {
    if (WiFi.status() != WL_CONNECTED) return -99;

    if (g_netMutex) xSemaphoreTake(g_netMutex, portMAX_DELAY);

    WiFiClientSecure client;
    client.setInsecure();
    client.setTimeout(8000);

    HTTPClient http;
    http.setTimeout(8000);

    if (!http.begin(client, String(API_BASE_URL) + path)) {
        Serial.printf("[NET] begin() failed: %s%s\n", API_BASE_URL, path);
        if (g_netMutex) xSemaphoreGive(g_netMutex);
        return -1;
    }

    http.addHeader("Content-Type", "application/json");
    http.addHeader("X-Api-Key",    API_KEY);

    int code = http.POST(body);
    if (code < 0) {
        Serial.printf("[NET] POST error %d freeHeap=%d\n", code, ESP.getFreeHeap());
    }

    http.end();
    client.stop();
    if (g_netMutex) xSemaphoreGive(g_netMutex);
    return code;
}

// taskWiFiUpload — calls from single task, mutex still protects shared WiFiClientSecure internals
inline int netIngestPost(const String& body) {
    return _httpsPost("/api/ingest", body);
}

// taskAlert + taskHeartbeat — mutex guarded
inline int netAlertPost(const char* path, const String& body) {
    return _httpsPost(path, body);
}
