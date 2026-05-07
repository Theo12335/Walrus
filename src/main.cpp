/**
 * =============================================================================
 * PROJECT WALRUS - PRODUCTION API INTEGRATION (v7.0)
 * =============================================================================
 * Data Flow: ESP32 -> Vercel API (HTTPS) -> Supabase
 * API Spec: production-v1 (device_id, sensors, actuators, state)
 * =============================================================================
 */

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <time.h>

// --- WiFi & API CONFIG ---
const char *ssid        = "Wassup2.4G";
const char *password    = "Bascon12335";
const char *api_key     = "walrus-esp32-key-2026";
const char *device_id   = "WALRUS_001";
const char *backend_url = "https://walrus-pi.vercel.app/api/esp32/data";

// --- NTP (Philippines UTC+8) ---
const char *ntp_server = "pool.ntp.org";
const long  gmt_offset = 8 * 3600;
const int   dst_offset = 0;

// --- SLEEP SCHEDULE ---
constexpr int SLEEP_HOUR = 20; // 8 PM — system sleeps
constexpr int WAKE_HOUR  = 6;  // 6 AM — system wakes

// --- PIN DEFINITIONS ---
constexpr uint8_t PIN_DS18B20            = 4;
constexpr uint8_t PIN_TDS_ANALOG         = 34;
constexpr uint8_t PIN_ULTRA_TRIG         = 19; // clean water output level
constexpr uint8_t PIN_ULTRA_ECHO         = 18;
constexpr uint8_t PIN_RELAY_PUMP_INTAKE  = 26; // IN1 — intake pump
constexpr uint8_t PIN_RELAY_PUMP_COLLECT = 27; // IN2 — collection pump
constexpr uint8_t PIN_RELAY_MIST         = 32; // IN3 — atomizer/mist
constexpr uint8_t PIN_FLOAT_SWITCH       = 33; // 2-wire ball float switch

// Clean water present if ultrasonic reads <= this distance (cm)
constexpr float CLEAN_WATER_THRESHOLD = 20.0f;

// --- GLOBAL STATE ---
float currentTempC     = 25.0;
float currentCleanDist = 999.0;
int   currentTds       = 0;
bool  isIntakePumpOn     = false;
bool  isCollectPumpOn    = false;
bool  isMistOn           = false;
bool  floatWaterDetected = false;
unsigned long lastCloudSync = 0;

// --- OVERRIDE STATE (set by mobile app via API response) ---
String overrideIntakePump  = "auto"; // "auto" | "on" | "off"
String overrideCollectPump = "auto";
String overrideMist        = "auto";

OneWire oneWire(PIN_DS18B20);
DallasTemperature sensors(&oneWire);

// --- DEEP SLEEP ---
void enterDeepSleep() {
    // Turn everything off before sleeping
    digitalWrite(PIN_RELAY_PUMP_INTAKE, HIGH);
    digitalWrite(PIN_RELAY_PUMP_COLLECT, HIGH);
    digitalWrite(PIN_RELAY_MIST, HIGH);

    struct tm timeinfo;
    uint64_t sleepSeconds = 8ULL * 3600; // fallback: 8 hours

    if (getLocalTime(&timeinfo)) {
        int secondsNow  = timeinfo.tm_hour * 3600 + timeinfo.tm_min * 60 + timeinfo.tm_sec;
        int wakeSeconds = WAKE_HOUR * 3600;
        sleepSeconds = (secondsNow < wakeSeconds)
            ? (uint64_t)(wakeSeconds - secondsNow)
            : (uint64_t)(24 * 3600 - secondsNow + wakeSeconds);
    }

    Serial.printf("[SLEEP] Sleeping for %llu s — waking at %02d:00\n", sleepSeconds, WAKE_HOUR);
    Serial.flush();
    esp_sleep_enable_timer_wakeup(sleepSeconds * 1000000ULL);
    esp_deep_sleep_start();
}

// --- SENSOR LOGIC ---
float getDistance(uint8_t trig, uint8_t echo) {
    const int samples = 5;
    float readings[samples];
    int valid = 0;

    for (int i = 0; i < samples; i++) {
        digitalWrite(trig, LOW);
        delayMicroseconds(2);
        digitalWrite(trig, HIGH);
        delayMicroseconds(10);
        digitalWrite(trig, LOW);
        long dur = pulseIn(echo, HIGH, 30000);
        if (dur > 0) {
            readings[valid++] = (dur * 0.0343f) / 2.0f;
        }
        delay(10);
    }

    if (valid == 0) return 999.0f;

    // return average of valid readings
    float sum = 0;
    for (int i = 0; i < valid; i++) sum += readings[i];
    return sum / valid;
}

void updateSensors() {
    // Temperature
    sensors.requestTemperatures();
    float t = sensors.getTempCByIndex(0);
    Serial.printf("[DS18B20] Raw: %.1f\n", t);
    if (t > -50 && t < 100) currentTempC = t;

    // Ultrasonic — clean water output level
    currentCleanDist = getDistance(PIN_ULTRA_TRIG, PIN_ULTRA_ECHO);

    // TDS
    uint32_t analogVal = analogRead(PIN_TDS_ANALOG);
    float v   = (analogVal / 4095.0f) * 3.3f;
    float raw = (133.42f * pow(v, 3) - 255.86f * pow(v, 2) + 857.39f * v) * 0.5f;
    currentTds = (int)(raw / (1.0f + 0.02f * (currentTempC - 25.0f)));

    // Float switch → controls intake pump (unless overridden)
    floatWaterDetected = (digitalRead(PIN_FLOAT_SWITCH) == LOW);
    if (overrideIntakePump == "on") {
        digitalWrite(PIN_RELAY_PUMP_INTAKE, LOW);
        isIntakePumpOn = true;
    } else if (overrideIntakePump == "off") {
        digitalWrite(PIN_RELAY_PUMP_INTAKE, HIGH);
        isIntakePumpOn = false;
    } else {
        if (!floatWaterDetected) {
            digitalWrite(PIN_RELAY_PUMP_INTAKE, LOW);
            isIntakePumpOn = true;
        } else {
            digitalWrite(PIN_RELAY_PUMP_INTAKE, HIGH);
            isIntakePumpOn = false;
        }
    }

    // Ultrasonic → controls collection pump (unless overridden)
    bool cleanWaterPresent = (currentCleanDist <= CLEAN_WATER_THRESHOLD);
    if (overrideCollectPump == "on") {
        digitalWrite(PIN_RELAY_PUMP_COLLECT, LOW);
        isCollectPumpOn = true;
    } else if (overrideCollectPump == "off") {
        digitalWrite(PIN_RELAY_PUMP_COLLECT, HIGH);
        isCollectPumpOn = false;
    } else {
        if (cleanWaterPresent) {
            digitalWrite(PIN_RELAY_PUMP_COLLECT, LOW);
            isCollectPumpOn = true;
        } else {
            digitalWrite(PIN_RELAY_PUMP_COLLECT, HIGH);
            isCollectPumpOn = false;
        }
    }

    // Mist/atomizer (unless overridden)
    if (overrideMist == "on") {
        digitalWrite(PIN_RELAY_MIST, LOW);
        isMistOn = true;
    } else if (overrideMist == "off") {
        digitalWrite(PIN_RELAY_MIST, HIGH);
        isMistOn = false;
    } else {
        if (currentTempC >= 30.0f && !isMistOn) {
            digitalWrite(PIN_RELAY_MIST, LOW);
            isMistOn = true;
        } else if (currentTempC < 28.0f && isMistOn) {
            digitalWrite(PIN_RELAY_MIST, HIGH);
            isMistOn = false;
        }
    }
}

// --- CLOUD SYNC ---
void syncWithProductionAPI() {
    if (WiFi.status() != WL_CONNECTED) return;

    WiFiClientSecure *client = new WiFiClientSecure;
    if (!client) return;
    client->setInsecure();

    HTTPClient http;
    if (http.begin(*client, backend_url)) {
        http.addHeader("Content-Type", "application/json");
        http.addHeader("X-API-Key", api_key);

        JsonDocument doc;
        doc["device_id"] = device_id;

        JsonObject sensorsObj = doc["sensors"].to<JsonObject>();
        sensorsObj["basin_temp"]     = currentTempC;
        sensorsObj["tds_ppm"]        = currentTds;
        sensorsObj["clean_level_cm"] = currentCleanDist;

        JsonObject actuatorsObj = doc["actuators"].to<JsonObject>();
        actuatorsObj["intake_pump_active"]  = isIntakePumpOn;
        actuatorsObj["collect_pump_active"] = isCollectPumpOn;
        actuatorsObj["mist_active"]         = isMistOn;
        actuatorsObj["float_water_detect"]  = floatWaterDetected;

        if (isIntakePumpOn)        doc["state"] = "Intake";
        else if (isCollectPumpOn)  doc["state"] = "Collecting";
        else                       doc["state"] = "Monitoring";

        String payload;
        serializeJson(doc, payload);
        Serial.println("[API] Syncing to Vercel:");
        Serial.println(payload);

        int code = http.POST(payload);
        if (code == 201) {
            Serial.println("Success (201 Created)");
        } else {
            Serial.printf("Error Code: %d\n", code);
            if (code == 401 || code == 403)
                Serial.println("WARNING: Check your X-API-Key!");
        }

        // Parse response for commands from mobile app
        String responseBody = http.getString();
        JsonDocument resDoc;
        if (deserializeJson(resDoc, responseBody) == DeserializationError::Ok) {
            if (resDoc["commands"]["intake_pump_override"].is<const char*>())
                overrideIntakePump  = resDoc["commands"]["intake_pump_override"].as<String>();
            if (resDoc["commands"]["collect_pump_override"].is<const char*>())
                overrideCollectPump = resDoc["commands"]["collect_pump_override"].as<String>();
            if (resDoc["commands"]["mist_override"].is<const char*>())
                overrideMist        = resDoc["commands"]["mist_override"].as<String>();

            Serial.printf("[CMD] Overrides — IN:%s COL:%s MIST:%s\n",
                overrideIntakePump.c_str(),
                overrideCollectPump.c_str(),
                overrideMist.c_str());
        }

        http.end();
    }
    delete client;
}

void setup() {
    // Relay pins — HIGH = OFF (active-low relay)
    pinMode(PIN_RELAY_PUMP_INTAKE, OUTPUT);
    digitalWrite(PIN_RELAY_PUMP_INTAKE, HIGH);

    pinMode(PIN_RELAY_PUMP_COLLECT, OUTPUT);
    digitalWrite(PIN_RELAY_PUMP_COLLECT, HIGH);

    pinMode(PIN_RELAY_MIST, OUTPUT);
    digitalWrite(PIN_RELAY_MIST, HIGH);

    pinMode(PIN_FLOAT_SWITCH, INPUT_PULLUP);

    pinMode(PIN_ULTRA_TRIG, OUTPUT);
    pinMode(PIN_ULTRA_ECHO, INPUT);

    Serial.begin(115200);
    sensors.begin();

    WiFi.begin(ssid, password);
    Serial.print("Connecting to Internet");
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.println("\nSystem Online.");

}

void loop() {
    static unsigned long lastLocal = 0;
    if (millis() - lastLocal >= 2000) {
        lastLocal = millis();
        updateSensors();
        Serial.printf("T:%.1f CLEAN:%.1f TDS:%d IN:%s COL:%s MIST:%s FLOAT:%s\n",
            currentTempC, currentCleanDist, currentTds,
            isIntakePumpOn  ? "ON" : "OFF",
            isCollectPumpOn ? "ON" : "OFF",
            isMistOn        ? "ON" : "OFF",
            floatWaterDetected ? "YES" : "NO");
    }

    if (millis() - lastCloudSync >= 15000) {
        lastCloudSync = millis();
        syncWithProductionAPI();
    }
}
