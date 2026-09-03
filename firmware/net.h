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

static String _extractHost() {
    String h(API_BASE_URL);
    if (h.startsWith("https://")) h.remove(0, 8);
    else if (h.startsWith("http://")) h.remove(0, 7);
    if (h.endsWith("/")) h.remove(h.length() - 1);
    return h;
}

static int _httpsPost(const char* path, const String& body) {
    if (WiFi.status() != WL_CONNECTED) return -99;

    if (g_netMutex) xSemaphoreTake(g_netMutex, portMAX_DELAY);

    String apiHost = _extractHost();

    WiFiClientSecure client;
    client.setInsecure();
    client.setTimeout(8000);

    HTTPClient http;
    http.setTimeout(8000);

    if (!http.begin(client, apiHost, (uint16_t)443, String(path), true)) {
        Serial.printf("[NET] begin() failed: %s freeHeap=%d\n",
                      apiHost.c_str(), ESP.getFreeHeap());
        http.end();
        client.stop();
        if (g_netMutex) xSemaphoreGive(g_netMutex);
        delay(200);
        if (g_netMutex) xSemaphoreTake(g_netMutex, portMAX_DELAY);

        WiFiClientSecure retryClient;
        retryClient.setInsecure();
        retryClient.setTimeout(8000);
        HTTPClient http2;
        http2.setTimeout(8000);

        if (!http2.begin(retryClient, apiHost, (uint16_t)443, String(path), true)) {
            Serial.printf("[NET] begin() failed (2nd try): %s freeHeap=%d\n",
                          apiHost.c_str(), ESP.getFreeHeap());
            http2.end();
            if (g_netMutex) xSemaphoreGive(g_netMutex);
            return -1;
        }

        http2.addHeader("Content-Type", "application/json");
        http2.addHeader("X-Api-Key",    API_KEY);

        int code = http2.POST(body);
        if (code < 0) {
            Serial.printf("[NET] POST error %d (%s) freeHeap=%d\n", code,
                          code == -1 ? "begin/TLS failed" : "unknown",
                          ESP.getFreeHeap());
        } else if (code >= 400) {
            Serial.printf("[NET] %s HTTP %d (auth 401 or 4xx/5xx — check API_KEY)\n",
                          String(path).c_str(), code);
        }

        http2.end();
        retryClient.stop();
        if (g_netMutex) xSemaphoreGive(g_netMutex);
        return code;
    }

    http.addHeader("Content-Type", "application/json");
    http.addHeader("X-Api-Key",    API_KEY);

    int code = http.POST(body);
    if (code < 0) {
        Serial.printf("[NET] POST error %d (%s) freeHeap=%d\n", code,
                      code == -1 ? "begin/TLS failed" : "unknown",
                      ESP.getFreeHeap());
    } else if (code >= 400) {
        Serial.printf("[NET] %s HTTP %d (auth 401 or 4xx/5xx — check API_KEY)\n",
                      String(path).c_str(), code);
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
