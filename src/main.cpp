/**
 * =============================================================================
 * PROJECT WALRUS - PRODUCTION API INTEGRATION (v8.0)
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

// --- WiFi & API CONFIG ---
const char *ssid        = "Wassup2.4G";
const char *password    = "Bascon12335";
const char *api_key     = "walrus-esp32-key-2026";
const char *device_id   = "WALRUS_001";
const char *backend_url = "https://walrus-pi.vercel.app/api/esp32/data";

// --- SLEEP SCHEDULE (disabled for testing) ---
constexpr int SLEEP_HOUR = 20;
constexpr int WAKE_HOUR  = 6;

// --- TIMING ---
constexpr unsigned long FAST_INTERVAL = 500;  // float, ultrasonic, TDS, pumps, API sync
constexpr unsigned long TEMP_INTERVAL = 5000; // DS18B20 (non-blocking)
constexpr unsigned long TEMP_WAIT_MS  = 800;  // wait after requestTemperatures()

// --- PIN DEFINITIONS ---
constexpr uint8_t PIN_DS18B20            = 22;
constexpr uint8_t PIN_TDS_ANALOG         = 34;
constexpr uint8_t PIN_ULTRA_TRIG         = 19;
constexpr uint8_t PIN_ULTRA_ECHO         = 21;
constexpr uint8_t PIN_RELAY_PUMP_INTAKE  = 26; // IN1
constexpr uint8_t PIN_RELAY_PUMP_COLLECT = 27; // IN2
constexpr uint8_t PIN_RELAY_MIST         = 25; // IN4 — atomizer/mist
constexpr uint8_t PIN_RELAY_HEATER       = 32; // IN3 — 12V PTC heater
constexpr uint8_t PIN_RELAY_PELTIER      = 23; // Peltier module (basin heat-up)
constexpr uint8_t PIN_FLOAT_SWITCH       = 33;

constexpr float CLEAN_WATER_THRESHOLD = 20.0f;
constexpr float HEATER_ON_TEMP        = 25.0f; // PTC: turn ON below this °C
constexpr float HEATER_OFF_TEMP       = 28.0f; // PTC: turn OFF above this °C
constexpr float PELTIER_ON_TEMP       = 30.0f; // Peltier: drive basin toward distillation temp
constexpr float PELTIER_OFF_TEMP      = 33.0f; // Peltier: stop heating once warm enough

// --- GLOBAL STATE ---
float currentTempC     = 25.0;
float currentCleanDist = 999.0;
int   currentTds       = 0;
bool  isIntakePumpOn     = false;
bool  isCollectPumpOn    = false;
bool  isMistOn           = false;
bool  isHeaterOn         = false;
bool  isPeltierOn        = false;
bool  floatWaterDetected = false;

// --- OVERRIDE STATE ---
String overrideIntakePump  = "auto";
String overrideCollectPump = "auto";
String overrideMist        = "auto";
String overridePeltier     = "auto";
// PTC Heater is auto-only (local thermostat). Not exposed to the API.

// --- SLEEP STATE (from app via response.sleep) ---
bool isSleeping = false;

// --- TEMP SENSOR STATE ---
bool tempRequested        = false;
unsigned long tempRequestTime = 0;

OneWire oneWire(PIN_DS18B20);
DallasTemperature sensors(&oneWire);

// --- ULTRASONIC ---
float getDistance(uint8_t trig, uint8_t echo) {
    const int samples = 3;
    float sum = 0;
    int valid = 0;
    for (int i = 0; i < samples; i++) {
        digitalWrite(trig, LOW);
        delayMicroseconds(2);
        digitalWrite(trig, HIGH);
        delayMicroseconds(10);
        digitalWrite(trig, LOW);
        long dur = pulseIn(echo, HIGH, 25000);
        if (dur > 0) {
            sum += (dur * 0.0343f) / 2.0f;
            valid++;
        }
        delay(10);
    }
    return (valid == 0) ? 999.0f : sum / valid;
}

// --- FAST SENSORS & ACTUATOR CONTROL ---
void updateFastSensors() {
    // Ultrasonic
    currentCleanDist = getDistance(PIN_ULTRA_TRIG, PIN_ULTRA_ECHO);

    // TDS
    uint32_t analogVal = analogRead(PIN_TDS_ANALOG);
    float v   = (analogVal / 4095.0f) * 3.3f;
    float raw = (133.42f * pow(v, 3) - 255.86f * pow(v, 2) + 857.39f * v) * 0.5f;
    currentTds = (int)(raw / (1.0f + 0.02f * (currentTempC - 25.0f)));

    // Float switch (always read — it's a sensor)
    floatWaterDetected = (digitalRead(PIN_FLOAT_SWITCH) == LOW);

    // SLEEP MODE: app told the device to power down — kill all relays, skip auto logic.
    // Heater & Peltier stay off too (active LOW relay → HIGH = off).
    if (isSleeping) {
        digitalWrite(PIN_RELAY_PUMP_INTAKE,  HIGH);
        digitalWrite(PIN_RELAY_PUMP_COLLECT, HIGH);
        digitalWrite(PIN_RELAY_MIST,         HIGH);
        digitalWrite(PIN_RELAY_HEATER,       HIGH);
        digitalWrite(PIN_RELAY_PELTIER,      HIGH);
        isIntakePumpOn = isCollectPumpOn = isMistOn = isHeaterOn = isPeltierOn = false;
        return;
    }
    // Intake pump — driven by float switch in auto, can be overridden from app
    if (overrideIntakePump == "on") {
        digitalWrite(PIN_RELAY_PUMP_INTAKE, LOW);
        isIntakePumpOn = true;
    } else if (overrideIntakePump == "off") {
        digitalWrite(PIN_RELAY_PUMP_INTAKE, HIGH);
        isIntakePumpOn = false;
    } else {
        bool on = !floatWaterDetected;
        digitalWrite(PIN_RELAY_PUMP_INTAKE, on ? LOW : HIGH);
        isIntakePumpOn = on;
    }

    // Ultrasonic → collection pump
    bool cleanWaterPresent = (currentCleanDist <= CLEAN_WATER_THRESHOLD);
    if (overrideCollectPump == "on") {
        digitalWrite(PIN_RELAY_PUMP_COLLECT, LOW);
        isCollectPumpOn = true;
    } else if (overrideCollectPump == "off") {
        digitalWrite(PIN_RELAY_PUMP_COLLECT, HIGH);
        isCollectPumpOn = false;
    } else {
        digitalWrite(PIN_RELAY_PUMP_COLLECT, cleanWaterPresent ? LOW : HIGH);
        isCollectPumpOn = cleanWaterPresent;
    }

    // Mist — ON >= 30°C, OFF < 28°C
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

    // PTC Heater — auto-only thermostat (not exposed to the API).
    // ON below 25°C, OFF above 28°C to keep the basin from freezing.
    if (currentTempC < HEATER_ON_TEMP && !isHeaterOn) {
        digitalWrite(PIN_RELAY_HEATER, LOW);
        isHeaterOn = true;
    } else if (currentTempC >= HEATER_OFF_TEMP && isHeaterOn) {
        digitalWrite(PIN_RELAY_HEATER, HIGH);
        isHeaterOn = false;
    }

    // Peltier — drives the basin to distillation temperature.
    // Auto: ON below 30°C, OFF above 33°C. Can be overridden from the app.
    if (overridePeltier == "on") {
        digitalWrite(PIN_RELAY_PELTIER, LOW);
        isPeltierOn = true;
    } else if (overridePeltier == "off") {
        digitalWrite(PIN_RELAY_PELTIER, HIGH);
        isPeltierOn = false;
    } else {
        if (currentTempC < PELTIER_ON_TEMP && !isPeltierOn) {
            digitalWrite(PIN_RELAY_PELTIER, LOW);
            isPeltierOn = true;
        } else if (currentTempC >= PELTIER_OFF_TEMP && isPeltierOn) {
            digitalWrite(PIN_RELAY_PELTIER, HIGH);
            isPeltierOn = false;
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

        // float_water_detect belongs in sensors per the WALRUS Pydantic SensorData model.
        // heater_active is intentionally omitted — the heater is a local auto-only thermostat.
        JsonObject sensorsObj = doc["sensors"].to<JsonObject>();
        sensorsObj["basin_temp"]         = currentTempC;
        sensorsObj["tds_ppm"]            = currentTds;
        sensorsObj["clean_level_cm"]     = currentCleanDist;
        sensorsObj["float_water_detect"] = floatWaterDetected;

        JsonObject actuatorsObj = doc["actuators"].to<JsonObject>();
        actuatorsObj["intake_pump_active"]  = isIntakePumpOn;
        actuatorsObj["collect_pump_active"] = isCollectPumpOn;
        actuatorsObj["mist_active"]         = isMistOn;
        // peltier_active will be stored once a `peltier_active BOOLEAN` column
        // is added to sensor_readings; until then the server silently drops it.
        actuatorsObj["peltier_active"]      = isPeltierOn;

        // State strings match the WALRUS StatusBadge / deviceStatus vocab.
        // Priority: Sleeping → Distilling (mist) → Collecting → Refilling → Monitoring.
        if (isSleeping)                doc["state"] = "Sleeping";
        else if (isMistOn)             doc["state"] = "Distilling";
        else if (isCollectPumpOn)      doc["state"] = "Collecting";
        else if (isIntakePumpOn)       doc["state"] = "Refilling";
        else                           doc["state"] = "Monitoring";

        String payload;
        serializeJson(doc, payload);
        Serial.println("[API] Syncing:");
        Serial.println(payload);

        int code = http.POST(payload);
        if (code == 201) {
            Serial.println("Success (201)");
        } else {
            Serial.printf("Error: %d\n", code);
            if (code == 401 || code == 403)
                Serial.println("WARNING: Check X-API-Key!");
        }

        // Parse commands from the app. Response shape (per WALRUS contract):
        //   { sleep: bool, commands: { intake_pump_override, collect_pump_override, mist_override } }
        String responseBody = http.getString();
        JsonDocument resDoc;
        if (deserializeJson(resDoc, responseBody) == DeserializationError::Ok) {
            if (resDoc["sleep"].is<bool>())
                isSleeping = resDoc["sleep"].as<bool>();

            if (resDoc["commands"]["intake_pump_override"].is<const char*>())
                overrideIntakePump  = resDoc["commands"]["intake_pump_override"].as<String>();
            if (resDoc["commands"]["collect_pump_override"].is<const char*>())
                overrideCollectPump = resDoc["commands"]["collect_pump_override"].as<String>();
            if (resDoc["commands"]["mist_override"].is<const char*>())
                overrideMist        = resDoc["commands"]["mist_override"].as<String>();
            if (resDoc["commands"]["peltier_override"].is<const char*>())
                overridePeltier     = resDoc["commands"]["peltier_override"].as<String>();

            Serial.printf("[CMD] sleep:%s IN:%s COL:%s MIST:%s PEL:%s\n",
                isSleeping ? "ON" : "OFF",
                overrideIntakePump.c_str(),
                overrideCollectPump.c_str(),
                overrideMist.c_str(),
                overridePeltier.c_str());
        }

        http.end();
    }
    delete client;
}

void setup() {
    pinMode(PIN_RELAY_PUMP_INTAKE, OUTPUT);
    digitalWrite(PIN_RELAY_PUMP_INTAKE, HIGH);

    pinMode(PIN_RELAY_PUMP_COLLECT, OUTPUT);
    digitalWrite(PIN_RELAY_PUMP_COLLECT, HIGH);

    pinMode(PIN_RELAY_MIST, OUTPUT);
    digitalWrite(PIN_RELAY_MIST, HIGH);

    pinMode(PIN_RELAY_HEATER, OUTPUT);
    digitalWrite(PIN_RELAY_HEATER, HIGH); // heater OFF on boot

    pinMode(PIN_RELAY_PELTIER, OUTPUT);
    digitalWrite(PIN_RELAY_PELTIER, HIGH); // peltier OFF on boot

    pinMode(PIN_FLOAT_SWITCH, INPUT_PULLUP);
    pinMode(PIN_ULTRA_TRIG, OUTPUT);
    pinMode(PIN_ULTRA_ECHO, INPUT);

    Serial.begin(115200);
    sensors.begin();
    sensors.setWaitForConversion(false);

    WiFi.begin(ssid, password);
    Serial.print("Connecting");
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.println("\nSystem Online.");
}

void loop() {
    unsigned long now = millis();

    // --- SLOW: DS18B20 temperature (non-blocking, every 5s) ---
    static unsigned long lastTempRequest = 0;
    if (!tempRequested && (now - lastTempRequest >= TEMP_INTERVAL)) {
        sensors.requestTemperatures();
        tempRequested   = true;
        tempRequestTime = now;
        lastTempRequest = now;
    }
    if (tempRequested && (now - tempRequestTime >= TEMP_WAIT_MS)) {
        float t = sensors.getTempCByIndex(0);
        Serial.printf("[DS18B20] %.1f C\n", t);
        if (t > -50 && t < 100) currentTempC = t;
        tempRequested = false;
    }

    // --- FAST: sensors, actuators, API sync (every 500ms) ---
    static unsigned long lastFast = 0;
    if (now - lastFast >= FAST_INTERVAL) {
        lastFast = now;
        updateFastSensors();
        Serial.printf("T:%.1f CLEAN:%.1f TDS:%d IN:%s COL:%s MIST:%s HEAT:%s PEL:%s FLOAT:%s SLEEP:%s\n",
            currentTempC, currentCleanDist, currentTds,
            isIntakePumpOn  ? "ON" : "OFF",
            isCollectPumpOn ? "ON" : "OFF",
            isMistOn        ? "ON" : "OFF",
            isHeaterOn      ? "ON" : "OFF",
            isPeltierOn     ? "ON" : "OFF",
            floatWaterDetected ? "YES" : "NO",
            isSleeping ? "YES" : "NO");
        syncWithProductionAPI();
    }
}
