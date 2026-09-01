// ================================================================
//  net.h — Persistent TLS connection helpers
//
//  Replaces the original httpsPost() which created a fresh
//  WiFiClientSecure (and full TLS handshake, ~300 ms) on every call.
//
//  Two persistent clients, one per logical path:
//    netIngestPost()  — taskWiFiUpload only    (no mutex needed)
//    netAlertPost()   — taskAlert + taskHB     (g_alertMutex guarded)
//
//  http.setReuse(true) keeps TCP/TLS alive after each request.
//  First POST still pays the handshake (~300 ms).
//  Every subsequent POST on the same connection: ~5–20 ms.
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

static WiFiClientSecure  g_ingestClient;
static WiFiClientSecure  g_alertClient;
static bool              g_ingestReady = false;
static bool              g_alertReady_ = false;   // distinct from g_alertReady flag
static SemaphoreHandle_t g_alertMutex  = nullptr;

inline void netInit() {
    g_alertMutex = xSemaphoreCreateMutex();
}

static int _httpsPostWith(WiFiClientSecure& client, bool& ready,
                           const char* path, const String& body) {
    if (WiFi.status() != WL_CONNECTED) return -99;

    if (!ready) {
        client.setInsecure();
        ready = true;
    }

    HTTPClient http;
    http.setReuse(true);    // keeps TCP/TLS alive after http.end()
    http.setTimeout(8000);

    if (!http.begin(client, String(API_BASE_URL) + path)) {
        client.stop(); ready = false;
        Serial.printf("[NET] begin() failed → TLS reset (%s)\n", path);
        return -1;
    }

    http.addHeader("Content-Type", "application/json");
    http.addHeader("X-Api-Key",    API_KEY);
    http.addHeader("Connection",   "keep-alive");

    int code = http.POST(body);

    if (code < 0) {
        client.stop(); ready = false;   // stale connection — reconnect next call
        Serial.printf("[NET] POST error %d heap=%d → TLS reset\n",
                      code, ESP.getFreeHeap());
    }

    http.end();   // releases HTTP layer; TCP/TLS stays open if keep-alive worked
    return code;
}

// taskWiFiUpload — single owner, no mutex
inline int netIngestPost(const String& body) {
    return _httpsPostWith(g_ingestClient, g_ingestReady, "/api/ingest", body);
}

// taskAlert + taskHeartbeat — shared, mutex guarded
inline int netAlertPost(const char* path, const String& body) {
    if (!g_alertMutex) return -1;
    xSemaphoreTake(g_alertMutex, portMAX_DELAY);
    int code = _httpsPostWith(g_alertClient, g_alertReady_, path, body);
    xSemaphoreGive(g_alertMutex);
    return code;
}
