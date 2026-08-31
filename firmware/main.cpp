// ================================================================
//  Seismic Monitoring Node  v2.0.0
//  Target MCU : ESP32 (Xtensa LX6 dual-core, 240 MHz, HW FPU)
//  Board      : ESP32 DevKitC (38-pin, WROOM-32)
//
//  Architecture: WiFi-only IoT seismic monitoring
//  Cloud       : Vercel serverless API + Upstash Redis
//
//  v2.0 changes vs v1.2
//  ─────────────────────
//  • NRF24L01+ removed → WiFi-only alert path
//  • BLE removed → simplified firmware, reduced power
//  • Dual-accelerometer weighted fusion (ADXL345 + MPU6050 accel)
//  • Complementary filter → quiescent tilt correction ONLY
//    (frozen during PGA > 0.005g to prevent seismic corruption)
//  • 5-second batch upload + immediate alert POST
//  • MicroSD logging on dedicated VSPI bus
//  • Empirical α selection from {0.90, 0.95, 0.98, 0.99}
//
// ──────────────────────────────────────────────────────────────────
//  PIN CONNECTIONS (ESP32 DevKitC, 38-pin)
// ──────────────────────────────────────────────────────────────────
//
//  ADXL345  ←→  VSPI (SPI3)
//  ┌────────┬──────────────────────────────────────────────────┐
//  │ ADXL   │ ESP32 GPIO                                     │
//  ├────────┼──────────────────────────────────────────────────┤
//  │ VCC    │ 3.3 V                                            │
//  │ GND    │ GND                                              │
//  │ CS     │ 15  (PIN_ADXL_CS)                               │
//  │ SCL/SCK│ 18  (VSPI SCK)                                  │
//  │ SDA/MOSI│23  (VSPI MOSI)                                  │
//  │ SDO/MISO│19  (VSPI MISO)                                  │
//  │ INT1   │ 33  (PIN_ADXL_INT1)                             │
//  └────────┴──────────────────────────────────────────────────┘
//
//  MPU6050  ←→  I2C (ESP32)
//  ┌────────┬──────────────────────────────────────────────────┐
//  │ MPU    │ ESP32 GPIO                                     │
//  ├────────┼──────────────────────────────────────────────────┤
//  │ VCC    │ 3.3 V                                            │
//  │ GND    │ GND                                              │
//  │ SDA    │ 21  (I2C SDA)                                   │
//  │ SCL    │ 22  (I2C SCL)                                   │
//  │ AD0    │ GND (I2C addr 0x68)                             │
//  └────────┴──────────────────────────────────────────────────┘
//  ⚠ Add 4.7 kΩ pull-up resistors on SDA and SCL to 3.3V
//
//  MicroSD  ←→  HSPI (SPI2)
//  ┌────────┬──────────────────────────────────────────────────┐
//  │ SD     │ ESP32 GPIO                                     │
//  ├────────┼──────────────────────────────────────────────────┤
//  │ VCC    │ 3.3 V                                            │
//  │ GND    │ GND                                              │
//  │ CS     │ 5   (PIN_SD_CS)                                 │
//  │ SCK    │ 14  (HSPI SCK)                                  │
//  │ MOSI   │ 13  (HSPI MOSI)                                 │
//  │ MISO   │ 12  (HSPI MISO)                                 │
//  └────────┴──────────────────────────────────────────────────┘
//
//  Flash: pio run -t upload
// ================================================================

#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <SD.h>
#include <ArduinoJson.h>
#include <math.h>
#include "ADXL345_WE.h"

#if __has_include("secrets.h")
#include "secrets.h"
#else
#define WIFI_SSID     ""
#define WIFI_PASS     ""
#define API_BASE_URL  ""
#define API_KEY       ""
#endif

// ── Pin definitions ───────────────────────────────────────────────
#define PIN_ADXL_CS     15
#define PIN_ADXL_SCK    18
#define PIN_ADXL_MOSI   23    //adxl sda
#define PIN_ADXL_MISO   19    //adxl sdo
#define PIN_ADXL_INT1   33

#define PIN_SD_CS        5
#define PIN_SD_SCK      14
#define PIN_SD_MOSI     13
#define PIN_SD_MISO     12

#define PIN_I2C_SDA     21
#define PIN_I2C_SCL     22
#define PIN_LED          2

#define MPU6050_ADDR    0x68

// ── ADXL345 registers ─────────────────────────────────────────────
// Register definitions are now provided by the ADXL345_WE library.

// ── MPU6050 registers ─────────────────────────────────────────────
#define MPU_ACCEL_XOUT_H 0x3B
#define MPU_GYRO_XOUT_H  0x43
#define MPU_PWR_MGMT_1   0x6B
#define MPU_GYRO_CONFIG  0x1B
#define MPU_ACCEL_CONFIG 0x1C
#define MPU_CONFIG       0x1A

// ── Signal-processing constants ───────────────────────────────────
constexpr float ADXL_SCALE_G   = 0.0039f;      // Eq 3.1: 3.9 mg/LSB
constexpr float MPU_SCALE_2G   = 1.0f / 16384.0f;  // ±2g mode
constexpr float MPU_SCALE_16G  = 1.0f / 2048.0f;   // ±16g mode
constexpr float MPU_GYRO_SCALE = (M_PI / 180.0f) / 131.0f; // ±250°/s
constexpr float SAMPLE_RATE_HZ = 200.0f;
constexpr float DT             = 1.0f / SAMPLE_RATE_HZ;

// ── Detection ─────────────────────────────────────────────────────
constexpr float    DETECT_THRESHOLD_G = 0.02f;
constexpr float    TILT_FREEZE_G      = 0.005f;  // Freeze comp filter above this
constexpr int      CONFIRM_SAMPLES    = 3;
constexpr uint32_t COOLDOWN_MS        = 500;

// ── Complementary-filter α (selected empirically in Phase B1) ─────
// Default 0.96; overridden if empirical test selects different value
float CF_ALPHA = 0.96f;

// ── Dual-accelerometer fusion weights (computed in Phase A1) ────
float W_ADXL = 0.88f;   // Eq 3.5
float W_MPU  = 0.12f;   // Eq 3.6

// ── Theoretical bare ADXL345 noise floor (Eq 3.17) ──────────────
constexpr float SIGMA_ADXL_BARE = 0.000943f;  // 943 µg

// ── Butterworth 2nd-order bandpass 0.5–40 Hz @ Fs=200 Hz ─────────
// Coefficients from MATLAB: [b,a] = butter(2, [0.5 40]/(200/2), 'bandpass')
struct Biquad {
    float b0, b1, b2, a1, a2, w1 = 0, w2 = 0;
    Biquad(float b0_, float b1_, float b2_, float a1_, float a2_)
        : b0(b0_), b1(b1_), b2(b2_), a1(a1_), a2(a2_) {}
    inline float process(float x) {
        float w0 = x - a1*w1 - a2*w2;
        float y  = b0*w0 + b1*w1 + b2*w2;
        w2 = w1; w1 = w0;
        return y;
    }
};

// Stage 1: HPF ~0.5 Hz (remove DC/tilt drift)
constexpr float H1_B0= 0.9305f, H1_B1=-1.8610f, H1_B2= 0.9305f, H1_A1=-1.8569f, H1_A2= 0.8651f;
// Stage 2: LPF ~40 Hz (anti-alias, remove high-freq noise)
constexpr float L1_B0= 0.1691f, L1_B1= 0.3383f, L1_B2= 0.1691f, L1_A1=-0.6655f, L1_A2= 0.1850f;

struct AxisFilter {
    Biquad hpf{H1_B0, H1_B1, H1_B2, H1_A1, H1_A2};
    Biquad lpf{L1_B0, L1_B1, L1_B2, L1_A1, L1_A2};
    inline float process(float x){ return lpf.process(hpf.process(x)); }
};

// ── SPI bus instances ─────────────────────────────────────────────
SPIClass vspi(VSPI);    // ADXL345  — SPI Mode 3 (bus 3), managed by ADXL345_WE
SPIClass hspi(HSPI);    // MicroSD  — SPI Mode 0 (bus 2)

// ADXL345 driver (wollewald/ADXL345_WE) on the VSPI bus.
// Routes VSPI to 18/23/19/15 (SCK, MOSI, MISO, CS).
ADXL345_WE adxl(&vspi, PIN_ADXL_CS, true, PIN_ADXL_MOSI, PIN_ADXL_MISO, PIN_ADXL_SCK);

// ── Alert payload (Core 0 → Core 1) ───────────────────────────────
struct AlertPayload {
    float    pga, ax_c, ay_c, az_c;
    float    sigma_fused, sigma_adxl, sigma_mpu;
    float    snr_db;
    uint32_t ts_ms;
};
volatile AlertPayload g_alert{};
volatile bool         g_alertReady = false;
SemaphoreHandle_t     g_alertSem;

// ── Live telemetry globals ────────────────────────────────────────
volatile float g_livePGA      = 0;
volatile float g_liveRoll     = 0;
volatile float g_livePitch    = 0;
volatile float g_liveSigmaF   = 0;   // Pipeline C (fused)
volatile float g_liveSigmaA   = 0;   // Pipeline A (ADXL only)
volatile float g_liveSigmaM   = 0;   // Pipeline B (MPU accel only)
volatile float g_liveSnrDb    = 0;

// ── Data-ready ISR ────────────────────────────────────────────────
volatile bool g_dataReady = false;
void IRAM_ATTR onDataReady() { g_dataReady = true; }

// ════════════════════════════════════════════════════════════════════
//  ADXL345 — handled by the ADXL345_WE library (see adxl global above)
// ════════════════════════════════════════════════════════════════════

static bool initADXL345() {
    // ADXL345_WE brings up the VSPI bus and applies full-res defaults.
    bool ini = adxl.init();
    Serial.printf("[ADXL345] init()=%s\n", ini ? "OK" : "FAIL");

    uint8_t devid = adxl.getDeviceID();
    Serial.printf("[ADXL345] DEVID=0x%02X (expect 0xE5)\n", devid);

    if (!ini) return false;

    adxl.setSPIClockSpeed(4000000);                   // 4 MHz (margin below 5 MHz max)
    if (!adxl.setDataRate(ADXL345_DATA_RATE_200)) { Serial.println("[ADXL345] setDataRate FAIL"); return false; } // 200 Hz ODR
    if (!adxl.setRange(ADXL345_RANGE_16G))      { Serial.println("[ADXL345] setRange FAIL"); return false; }      // ±16g (full-res kept)
    if (!adxl.setInterrupt(ADXL345_DATA_READY, INT_PIN_1)) { Serial.println("[ADXL345] setInterrupt FAIL"); return false; } // DATA_READY → INT1
    if (!adxl.isConnected())                    { Serial.println("[ADXL345] isConnected FAIL (DEVID mismatch)"); return false; } // DEVID == 0xE5

    Serial.println("[ADXL345] OK — 200 Hz, ±16g, full-res, INT1 (ADXL345_WE)");
    return true;
}

// ════════════════════════════════════════════════════════════════════
//  I2C bus scan
// ════════════════════════════════════════════════════════════════════
static void scanI2C() {
    Serial.println("[I2C] Scanning bus…");
    uint8_t count = 0;
    for (uint8_t addr = 1; addr < 127; addr++) {
        Wire.beginTransmission(addr);
        if (Wire.endTransmission() == 0) {
            Serial.printf("[I2C]   found 0x%02X\n", addr);
            count++;
        }
    }
    if (count == 0)
        Serial.println("[I2C]   NO devices found — check SDA/SCL wiring + pull-ups + power");
    else
        Serial.printf("[I2C]   %u device(s) on bus\n", count);
}

// ════════════════════════════════════════════════════════════════════
//  MPU6050 I2C helpers
// ════════════════════════════════════════════════════════════════════
static void mpuWrite(uint8_t reg, uint8_t val) {
    Wire.beginTransmission(MPU6050_ADDR);
    Wire.write(reg); Wire.write(val);
    Wire.endTransmission();
}

static void mpuReadAccel(int16_t& ax, int16_t& ay, int16_t& az) {
    Wire.beginTransmission(MPU6050_ADDR);
    Wire.write(MPU_ACCEL_XOUT_H);
    Wire.endTransmission(false);
    Wire.requestFrom((uint8_t)MPU6050_ADDR, (uint8_t)6, (uint8_t)true);
    ax = ((int16_t)Wire.read() << 8) | Wire.read();
    ay = ((int16_t)Wire.read() << 8) | Wire.read();
    az = ((int16_t)Wire.read() << 8) | Wire.read();
}

static void mpuReadGyro(float& gx, float& gy, float& gz) {
    Wire.beginTransmission(MPU6050_ADDR);
    Wire.write(MPU_GYRO_XOUT_H);
    Wire.endTransmission(false);
    Wire.requestFrom((uint8_t)MPU6050_ADDR, (uint8_t)6, (uint8_t)true);
    int16_t r[3];
    r[0] = ((int16_t)Wire.read() << 8) | Wire.read();
    r[1] = ((int16_t)Wire.read() << 8) | Wire.read();
    r[2] = ((int16_t)Wire.read() << 8) | Wire.read();
    gx = r[0] * MPU_GYRO_SCALE;
    gy = r[1] * MPU_GYRO_SCALE;
    gz = r[2] * MPU_GYRO_SCALE;
}

static bool initMPU6050() {
    Wire.beginTransmission(MPU6050_ADDR);
    if (Wire.endTransmission() != 0) {
        Serial.printf("[MPU6050] NACK at address 0x%02X — no device responds\n", MPU6050_ADDR);
        return false;
    }
    mpuWrite(MPU_PWR_MGMT_1, 0x00); delay(10);  // wake
    mpuWrite(MPU_GYRO_CONFIG,  0x00);           // ±250°/s
    mpuWrite(MPU_CONFIG,       0x03);           // DLPF 42 Hz
    // For quiescent noise characterisation: ±2g (AFS_SEL=0)
    // For shake table: switch to ±16g (AFS_SEL=3) in setup
    mpuWrite(MPU_ACCEL_CONFIG, 0x00);           // ±2g for now
    Wire.beginTransmission(MPU6050_ADDR);
    Wire.write(0x75);
    Wire.endTransmission(false);
    Wire.requestFrom((uint8_t)MPU6050_ADDR, (uint8_t)1, (uint8_t)true);
    uint8_t whoami = Wire.read();
    Serial.printf("[MPU6050] WHO_AM_I = 0x%02X (expect 0x68 MPU6050 / 0x70 MPU6500)\n", whoami);
    if (whoami != 0x68 && whoami != 0x70) return false;
    Serial.println("[MPU6050] OK — ±2g accel, ±250°/s gyro, DLPF 42 Hz");
    return true;
}

// ════════════════════════════════════════════════════════════════════
//  WiFi helpers
// ════════════════════════════════════════════════════════════════════
static void wifiReconnect() {
    if (WiFi.status() == WL_CONNECTED) return;
    WiFi.disconnect(true); delay(500);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) delay(300);
}

static SemaphoreHandle_t g_netMutex = nullptr;

static int httpsPost(const char* path, const String& body) {
    if (g_netMutex) xSemaphoreTake(g_netMutex, portMAX_DELAY);
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    if (!http.begin(client, String(API_BASE_URL) + path)) {
        if (g_netMutex) xSemaphoreGive(g_netMutex);
        return -1;
    }
    http.addHeader("Content-Type", "application/json");
    http.addHeader("X-Api-Key", API_KEY);
    http.setTimeout(8000);
    int code = http.POST(body);
    http.end();
    if (g_netMutex) xSemaphoreGive(g_netMutex);
    return code;
}

// ════════════════════════════════════════════════════════════════════
//  MicroSD helpers
// ════════════════════════════════════════════════════════════════════
static bool sdAvailable = false;

static bool initSD() {
    hspi.begin(PIN_SD_SCK, PIN_SD_MISO, PIN_SD_MOSI, PIN_SD_CS);
    if (!SD.begin(PIN_SD_CS, hspi, 8000000)) {
        Serial.println("[SD] Card init failed — check HSPI wiring");
        return false;
    }
    Serial.println("[SD] OK — 8 MHz HSPI");
    sdAvailable = true;
    return true;
}

static void sdLog(const String& line) {
    if (!sdAvailable) return;
    File f = SD.open("/seismic.csv", FILE_APPEND);
    if (f) { f.println(line); f.close(); }
}

// ════════════════════════════════════════════════════════════════════
//  CORE 0 — 200 Hz sensor task (Pipeline A, B, C)
// ════════════════════════════════════════════════════════════════════
static void taskSensor(void*) {
    // ── Filter state for all three pipelines ──────────────────────
    AxisFilter fXa, fYa, fZa;   // Pipeline A: ADXL345 only
    AxisFilter fXb, fYb, fZb;   // Pipeline B: MPU6050 accel only
    AxisFilter fXc, fYc, fZc;   // Pipeline C: fused (uses same input as A for now)

    // ── Complementary filter tilt state ─────────────────────────
    float phi = 0.0f, theta = 0.0f;
    bool tiltFrozen = false;

    // ── Detection FSM ─────────────────────────────────────────────
    enum { IDLE, CONFIRMED, COOLDOWN } state = IDLE;
    int      confirmN  = 0;
    uint32_t cooldownT = 0;
    float    axCached  = 0, ayCached = 0, azCached = 0;

    // ── Welford online σ — Pipeline A (ADXL only) ────────────────
    float    wMa = 0, wM2a = 0; uint32_t wNa = 0;
    // ── Welford online σ — Pipeline B (MPU accel only) ───────────
    float    wMb = 0, wM2b = 0; uint32_t wNb = 0;
    // ── Welford online σ — Pipeline C (fused) ────────────────────
    float    wMc = 0, wM2c = 0; uint32_t wNc = 0;

    auto welford = [](float x, float& mean, float& M2, uint32_t& N) -> float {
        ++N;
        float d = x - mean;
        mean += d / (float)N;
        M2   += d * (x - mean);
        return (N >= 50) ? sqrtf(M2 / (float)(N - 1)) : SIGMA_ADXL_BARE;
    };

    TickType_t wake = xTaskGetTickCount();

    while (true) {
        // Wait for ADXL345 DATA_READY on INT1 (max 6 ms)
        uint32_t t0 = millis();
        while (!g_dataReady && (millis() - t0 < 6)) vTaskDelay(1);
        g_dataReady = false;

        // ── Read ADXL345 (Eq 3.1) ────────────────────────────────
        xyzFloat adxlRaw;
        adxl.getRawValues(&adxlRaw);
        int16_t rxa = (int16_t)adxlRaw.x;
        int16_t rya = (int16_t)adxlRaw.y;
        int16_t rza = (int16_t)adxlRaw.z;
        float ax_adxl = rxa * ADXL_SCALE_G;
        float ay_adxl = rya * ADXL_SCALE_G;
        float az_adxl = rza * ADXL_SCALE_G;

        // ── Read MPU6050 accel + gyro ─────────────────────────────
        int16_t rxm, rym, rzm;
        mpuReadAccel(rxm, rym, rzm);
        float ax_mpu = rxm * MPU_SCALE_2G;  // ±2g mode for max sensitivity
        float ay_mpu = rym * MPU_SCALE_2G;
        float az_mpu = rzm * MPU_SCALE_2G;

        float gx, gy, gz;
        mpuReadGyro(gx, gy, gz);

        // ── PIPELINE A: ADXL345 only ─────────────────────────────
        float fxa = fXa.process(ax_adxl);
        float fya = fYa.process(ay_adxl);
        float fza = fZa.process(az_adxl) - 1.0f;
        float pga_a = sqrtf(fxa*fxa + fya*fya + fza*fza);
        if (wNa >= 120000) { wMa = 0; wM2a = 0; wNa = 0; }
        float sigma_a = welford(pga_a, wMa, wM2a, wNa);

        // ── PIPELINE B: MPU6050 accel only ────────────────────────
        float fxb = fXb.process(ax_mpu);
        float fyb = fYb.process(ay_mpu);
        float fzb = fZb.process(az_mpu) - 1.0f;
        float pga_b = sqrtf(fxb*fxb + fyb*fyb + fzb*fzb);
        if (wNb >= 120000) { wMb = 0; wM2b = 0; wNb = 0; }
        float sigma_b = welford(pga_b, wMb, wM2b, wNb);

        // ── PIPELINE C: Dual-accelerometer weighted fusion ───────
        // Eq 3.4: weighted fusion of bandpass-filtered accelerations
        float fxc_raw = W_ADXL * fxa + W_MPU * fxb;
        float fyc_raw = W_ADXL * fya + W_MPU * fyb;
        float fzc_raw = W_ADXL * fza + W_MPU * fzb;

        // ── Complementary filter: quiescent tilt correction ──────
        // Eq 3.9–3.10: tilt from accelerometer
        float phiA   = atan2f(ay_adxl,  sqrtf(ax_adxl*ax_adxl + az_adxl*az_adxl));
        float thetaA = atan2f(-ax_adxl, sqrtf(ay_adxl*ay_adxl + az_adxl*az_adxl));

        // Freeze tilt during seismic activity (PGA > TILT_FREEZE_G)
        if (pga_a < TILT_FREEZE_G && !tiltFrozen) {
            // Eq 3.11–3.12: normal complementary filter update
            phi   = CF_ALPHA * (phi   + gx * DT) + (1.0f - CF_ALPHA) * phiA;
            theta = CF_ALPHA * (theta + gy * DT) + (1.0f - CF_ALPHA) * thetaA;
        } else if (pga_a >= TILT_FREEZE_G) {
            tiltFrozen = true;  // freeze on first exceedance
        } else if (pga_a < TILT_FREEZE_G && tiltFrozen) {
            // Unfreeze only after 1 second of quiescence
            static uint32_t unfreezeT = 0;
            if (unfreezeT == 0) unfreezeT = millis();
            if (millis() - unfreezeT > 1000) {
                tiltFrozen = false;
                unfreezeT = 0;
                // Re-seed with current accelerometer estimate
                phi = phiA; theta = thetaA;
            }
        }

        // Eq 3.13–3.15: rotate fused acceleration using frozen/quiescent tilt
        float sp = sinf(theta), cp = cosf(theta);
        float sr = sinf(phi),   cr = cosf(phi);
        float axc =  fxc_raw * cp + fzc_raw * sp;
        float ayc =  fyc_raw * cr - fzc_raw * sr;
        float azc = -fxc_raw * sp + fzc_raw * cp - 1.0f;

        // Eq 3.16: PGA
        float pga_c = sqrtf(axc*axc + ayc*ayc + azc*azc);

        if (wNc >= 120000) { wMc = 0; wM2c = 0; wNc = 0; }
        float sigma_c = welford(pga_c, wMc, wM2c, wNc);
        float snr_db = sigma_c > 0 ? 20.0f * log10f(DETECT_THRESHOLD_G / sigma_c) : 0.0f;

        // ── Publish telemetry ─────────────────────────────────────
        g_livePGA    = pga_c;
        g_liveRoll   = phi   * (180.0f / M_PI);
        g_livePitch  = theta * (180.0f / M_PI);
        g_liveSigmaF = sigma_c;
        g_liveSigmaA = sigma_a;
        g_liveSigmaM = sigma_b;
        g_liveSnrDb  = snr_db;

        // ── Live telemetry print (~1 Hz) ─────────────────────────
        static uint32_t lastPrint = 0;
        if (millis() - lastPrint >= 1000) {
            lastPrint = millis();
            Serial.printf("[LIVE] PGA=%.5fg sigma_f=%.6fg sigma_a=%.6fg sigma_m=%.6fg SNR=%.1fdB roll=%.2f pitch=%.2f\n",
                g_livePGA, g_liveSigmaF, g_liveSigmaA, g_liveSigmaM, g_liveSnrDb, g_liveRoll, g_livePitch);
        }

        // ── Detection FSM (Pipeline C PGA) ───────────────────────
        switch (state) {
            case IDLE:
                if (pga_c >= DETECT_THRESHOLD_G) {
                    axCached = axc; ayCached = ayc; azCached = azc;
                    if (++confirmN >= CONFIRM_SAMPLES) {
                        AlertPayload alert = {
                            pga_c, axCached, ayCached, azCached,
                            sigma_c, sigma_a, sigma_b, snr_db, millis()
                        };
                        memcpy((void*)&g_alert, &alert, sizeof(alert));
                        g_alertReady = true;
                        xSemaphoreGive(g_alertSem);
                        state = CONFIRMED;
                    }
                } else {
                    confirmN = 0;
                }
                break;
            case CONFIRMED:
                cooldownT = millis(); state = COOLDOWN;
                break;
            case COOLDOWN:
                if (millis() - cooldownT >= COOLDOWN_MS) {
                    state = IDLE; confirmN = 0;
                }
                break;
        }

        vTaskDelayUntil(&wake, pdMS_TO_TICKS(5));
    }
}

// ════════════════════════════════════════════════════════════════════
//  CORE 1 TASK A — WiFi continuous upload (5-second batches)
// ════════════════════════════════════════════════════════════════════
struct SampleRec {
    uint32_t ts;
    float pga_c, roll, pitch, sigma_f, sigma_a, sigma_m, snr_db;
};

static SampleRec sRing[1000];  // 5 seconds @ 200 Hz
static volatile int sHead = 0, sTail = 0;

static void taskWiFiUpload(void*) {
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    {
        uint32_t t = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - t < 15000) delay(300);
    }
    Serial.printf("[WiFi] %s\n",
        WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString().c_str() : "OFFLINE");

    uint32_t lastUpload = 0, lastWifi = 0;

    while (true) {
        // Enqueue sample at 200 Hz (called every 5 ms from sensor task via shared ring)
        // Actually: sensor task writes directly; this task just uploads batches
        // We poll g_live* at 10 Hz for the ring buffer
        static uint32_t lastSample = 0;
        if (millis() - lastSample >= 5) {
            lastSample = millis();
            int next = (sHead + 1) % 1000;
            if (next != sTail) {
                sRing[sHead] = {
                    millis(),
                    g_livePGA, g_liveRoll, g_livePitch,
                    g_liveSigmaF, g_liveSigmaA, g_liveSigmaM, g_liveSnrDb
                };
                sHead = next;
            }
        }

        if (millis() - lastWifi > 30000) { lastWifi = millis(); wifiReconnect(); }

        // 5-second batch upload
        if (millis() - lastUpload >= 5000 && WiFi.status() == WL_CONNECTED) {
            lastUpload = millis();
            DynamicJsonDocument doc(12288);
            doc["node_id"]     = "ADXL345-01";
            doc["fw"]          = "2.0.0";
            doc["threshold_g"] = DETECT_THRESHOLD_G;
            doc["cf_alpha"]    = CF_ALPHA;
            doc["w_adxl"]      = W_ADXL;
            doc["w_mpu"]       = W_MPU;

            JsonArray arr = doc.createNestedArray("samples");
            int cnt = 0;
            while (sTail != sHead && cnt < 200) {
                JsonObject s = arr.createNestedObject();
                s["ts"]      = sRing[sTail].ts;
                s["pga_c"]   = serialized(String(sRing[sTail].pga_c,  5));
                s["roll"]    = serialized(String(sRing[sTail].roll,    2));
                s["pitch"]   = serialized(String(sRing[sTail].pitch,   2));
                s["sigma_f"] = serialized(String(sRing[sTail].sigma_f, 5));
                s["sigma_a"] = serialized(String(sRing[sTail].sigma_a, 5));
                s["sigma_m"] = serialized(String(sRing[sTail].sigma_m, 5));
                s["snr_db"]  = serialized(String(sRing[sTail].snr_db,  2));
                sTail = (sTail + 1) % 1000;
                cnt++;
            }
            if (cnt) {
                String body; serializeJson(doc, body);
                int code = httpsPost("/api/ingest", body);
                Serial.printf("[WiFi] %d samples → HTTP %d\n", cnt, code);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// ════════════════════════════════════════════════════════════════════
//  CORE 1 TASK B — Immediate alert POST + SD log
// ════════════════════════════════════════════════════════════════════
static void taskAlert(void*) {
    while (true) {
        if (xSemaphoreTake(g_alertSem, portMAX_DELAY) != pdTRUE) continue;

        AlertPayload al;
        memcpy((void*)&al, (const void*)&g_alert, sizeof(AlertPayload));

        // Compute empirical η (Eq 4.2) and % reduction (Eq 4.3)
        float eta = 0.0f, pct = 0.0f;
        if (al.sigma_fused < al.sigma_adxl && al.sigma_adxl > 0) {
            eta = 1.0f - (al.sigma_fused * al.sigma_fused) / (al.sigma_adxl * al.sigma_adxl);
            pct = (al.sigma_adxl - al.sigma_fused) / al.sigma_adxl * 100.0f;
        }

        // ── Immediate WiFi POST ──────────────────────────────────
        if (WiFi.status() == WL_CONNECTED) {
            StaticJsonDocument<512> doc;
            doc["node_id"]             = "ADXL345-01";
            doc["event_type"]          = "seismic_detection";
            doc["pga_mgal"]            = (int)(al.pga * 1000.0f);
            doc["pga"]                 = serialized(String(al.pga,        5));
            doc["ax_corr"]             = serialized(String(al.ax_c,       5));
            doc["ay_corr"]             = serialized(String(al.ay_c,       5));
            doc["az_corr"]             = serialized(String(al.az_c,       5));
            doc["ts_ms"]               = al.ts_ms;
            doc["sigma_fused"]         = serialized(String(al.sigma_fused, 5));
            doc["sigma_adxl"]          = serialized(String(al.sigma_adxl,  5));
            doc["sigma_mpu"]           = serialized(String(al.sigma_mpu,   5));
            doc["snr_db"]              = serialized(String(al.snr_db,      2));
            doc["noise_reduction_eta"] = serialized(String(eta,            3));
            doc["noise_reduction_pct"] = serialized(String(pct,            1));
            String body; serializeJson(doc, body);
            int code = httpsPost("/api/alerts", body);
            Serial.printf("[Cloud] Alert POST → HTTP %d | PGA=%.5fg η=%.3f\n",
                          code, al.pga, eta);
        }

        // ── SD card log ──────────────────────────────────────────
        char csv[256];
        snprintf(csv, sizeof(csv),
            "%lu,ALERT,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%.1f,%.3f,%.1f",
            al.ts_ms, al.pga, al.ax_c, al.ay_c, al.az_c,
            al.sigma_fused, al.sigma_adxl, al.sigma_mpu,
            al.snr_db, eta, pct);
        sdLog(String(csv));

        // 3 × blink on alert
        for (int i = 0; i < 3; i++) {
            digitalWrite(PIN_LED, HIGH); delay(80);
            digitalWrite(PIN_LED, LOW);  delay(80);
        }
        g_alertReady = false;
    }
}

// ════════════════════════════════════════════════════════════════════
//  CORE 1 TASK C — Heartbeat + SD continuous log
// ════════════════════════════════════════════════════════════════════
static void taskHeartbeat(void*) {
    uint32_t lastBeat = 0;
    while (true) {
        if (millis() - lastBeat >= 60000) {
            lastBeat = millis();
            // Heartbeat POST
            if (WiFi.status() == WL_CONNECTED) {
                StaticJsonDocument<256> doc;
                doc["node_id"]   = "ADXL345-01";
                doc["ts_ms"]     = millis();
                doc["status"]    = "alive";
                doc["rssi_dbm"]  = WiFi.RSSI();
                doc["free_heap"] = ESP.getFreeHeap();
                String body; serializeJson(doc, body);
                httpsPost("/api/heartbeat", body);
            }
            // SD heartbeat
            char csv[128];
            snprintf(csv, sizeof(csv), "%lu,HEARTBEAT,%d,%d",
                     millis(), WiFi.RSSI(), ESP.getFreeHeap());
            sdLog(String(csv));
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

// ════════════════════════════════════════════════════════════════════
//  SETUP
// ════════════════════════════════════════════════════════════════════
void setup() {
    Serial.begin(115200); delay(500);
    Serial.println("\n[BOOT] Seismic Monitoring Node v2.0.0");
    Serial.println("[BOOT] Board: ESP32 DevKitC (38-pin, WROOM-32)");
    Serial.println("[BOOT] Arch:  WiFi-only IoT cloud monitoring");
    Serial.println("[BOOT] Fusion: Dual-accelerometer weighted (ADXL345 + MPU6050)");

    pinMode(PIN_LED,     OUTPUT);
    pinMode(PIN_SD_CS,   OUTPUT);
    digitalWrite(PIN_SD_CS,   HIGH);
    
    pinMode(PIN_ADXL_CS, OUTPUT);
    digitalWrite(PIN_ADXL_CS, HIGH);  // CS is active-low, so HIGH = inactive

    delay(100);  // Give CS pin time to stabilize before ADXL345_WE initializes

    // ADXL345 is initialised on the VSPI bus by ADXL345_WE inside initADXL345().

    // I2C for MPU6050
    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
    Wire.setClock(400000);
    delay(10);
    scanI2C();

    if (!initADXL345()) {
        Serial.println("[FATAL] ADXL345 not detected — check VSPI wiring");
        while (true) { digitalWrite(PIN_LED, !digitalRead(PIN_LED)); delay(200); }
    }
    if (!initMPU6050()) {
        Serial.println("[FATAL] MPU6050 not detected — check I2C wiring");
        while (true) { digitalWrite(PIN_LED, !digitalRead(PIN_LED)); delay(500); }
    }
    // initSD();  // uncomment when SD card is wired up

    attachInterrupt(digitalPinToInterrupt(PIN_ADXL_INT1), onDataReady, RISING);

    g_alertSem = xSemaphoreCreateBinary();
    g_netMutex = xSemaphoreCreateMutex();

    // Core 0: time-critical 200 Hz sensor acquisition (highest priority)
    xTaskCreatePinnedToCore(taskSensor,   "Sensor",   8192, nullptr,
                            configMAX_PRIORITIES - 1, nullptr, 0);
    // Core 1: WiFi 5-second batch upload
    xTaskCreatePinnedToCore(taskWiFiUpload, "WiFiUp", 16384, nullptr,
                            configMAX_PRIORITIES - 3, nullptr, 1);
    // Core 1: Immediate alert POST + SD log
    xTaskCreatePinnedToCore(taskAlert,      "Alert",  8192, nullptr,
                            configMAX_PRIORITIES - 2, nullptr, 1);
    // Core 1: 60-second heartbeat
    xTaskCreatePinnedToCore(taskHeartbeat,  "HB",     8192, nullptr,
                            configMAX_PRIORITIES - 4, nullptr, 1);

    Serial.println("[BOOT] Running — 3 pipelines (A/B/C) at 200 Hz");
    digitalWrite(PIN_LED, HIGH); delay(150); digitalWrite(PIN_LED, LOW);
}

void loop() {
    vTaskDelay(pdMS_TO_TICKS(60000));
}
