/**
 * =============================================================================
 * PROJECT WALRUS - PRODUCTION API INTEGRATION (v12.0)
 * =============================================================================
 * Data Flow: ESP32 -> Vercel API (HTTPS) -> Supabase
 * Timezone: Philippines Standard Time (UTC+8, no DST)
 *
 * Operating window: 08:01–17:59 (PST)
 * Sleep window:     18:00–08:00 — all relays OFF, sensors still read
 *
 * Sensors (data only):
 *   DS18B20    — basin temperature
 *   TDS        — water quality
 *   Float switch — water presence in basin
 *
 * Actuators:
 *   Intake pump  — ON when float switch detects no water, OFF when water present
 *   Collect pump — 5 s ON every 30 min (suspended while peltier is active)
 *   Peltier      — 10:30–15:00, 12 min ON / 18 min OFF cycle
 *                  When peltier activates: all other relays go OFF.
 *                  If basin is empty, intake pump fills it before peltier fires.
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
const char *ssid = "Theo";
const char *password = "wassuppp";
const char *api_key = "walrus-esp32-key-2026";
const char *device_id = "WALRUS_001";
const char *backend_url = "https://walrus-pi.vercel.app/api/esp32/data";

// --- TIMING ---
constexpr unsigned long FAST_INTERVAL = 500;
constexpr unsigned long TEMP_INTERVAL = 5000;
constexpr unsigned long TEMP_WAIT_MS = 800;

// Collection pump: 5 s ON every 30 min
constexpr unsigned long COLLECT_ON_MS = 5000UL;
constexpr unsigned long COLLECT_CYCLE_MS = 30UL * 60UL * 1000UL;

// Operating window: 08:01–17:59 PST
constexpr int WAKE_MIN = 8 * 60 + 1; // 481
constexpr int SLEEP_MIN = 18 * 60;   // 1080

// Peltier schedule: 10:30–15:00, 12 min ON / 18 min OFF (30-min cycle)
constexpr int PELTIER_START_MIN = 10 * 60 + 30;
constexpr int PELTIER_STOP_MIN = 15 * 60;
constexpr int PELTIER_ON_MIN = 12;
constexpr int PELTIER_CYCLE_MIN = 30;

// --- PIN DEFINITIONS ---
constexpr uint8_t PIN_DS18B20 = 22;
constexpr uint8_t PIN_TDS_ANALOG = 34;
constexpr uint8_t PIN_RELAY_PUMP_INTAKE = 26;  // IN1
constexpr uint8_t PIN_RELAY_PUMP_COLLECT = 27; // IN2
constexpr uint8_t PIN_RELAY_PELTIER = 32;      // IN3
constexpr uint8_t PIN_FLOAT_SWITCH = 14;

// --- GLOBAL STATE ---
float currentTempC = 25.0;
int currentTds = 0;
bool isIntakePumpOn = false;
bool isCollectPumpOn = false;
bool isPeltierOn = false;
bool floatWaterDetected = false;

// --- OVERRIDE STATE (from app) ---
String overrideIntakePump = "auto";
String overrideCollectPump = "auto";
String overridePeltier = "auto";

// isSleeping: set by the app for manual sleep commands.
// Time-based sleep is enforced separately and cannot be overridden by the app.
bool isSleeping = false;

// --- TEMP SENSOR STATE ---
bool tempRequested = false;
unsigned long tempRequestTime = 0;

// --- COLLECTION PUMP CYCLE ---
unsigned long collectCycleStart = 0;

OneWire oneWire(PIN_DS18B20);
DallasTemperature sensors(&oneWire);

// True during the system sleep window (18:00–08:00 PST).
// Returns false if NTP has not synced yet (safe default: stay awake).
bool isInSleepHours()
{
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo))
        return false;
    int totalMin = timeinfo.tm_hour * 60 + timeinfo.tm_min;
    return (totalMin < WAKE_MIN || totalMin >= SLEEP_MIN);
}

// True when the peltier should be in its ON phase (10:30–15:00, 12/18-min cycle).
// Returns false outside the operating window or if NTP has not synced.
bool peltierShouldRun()
{
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo))
        return false;
    int totalMin = timeinfo.tm_hour * 60 + timeinfo.tm_min;
    if (totalMin < PELTIER_START_MIN || totalMin >= PELTIER_STOP_MIN)
        return false;
    int cycleMin = (totalMin - PELTIER_START_MIN) % PELTIER_CYCLE_MIN;
    return cycleMin < PELTIER_ON_MIN;
}

// --- FAST SENSORS & ACTUATOR CONTROL ---
void updateFastSensors()
{
    unsigned long now = millis();

    // TDS — data only
    uint32_t analogVal = analogRead(PIN_TDS_ANALOG);
    float v = (analogVal / 4095.0f) * 3.3f;
    float raw = (133.42f * pow(v, 3) - 255.86f * pow(v, 2) + 857.39f * v) * 0.5f;
    currentTds = (int)(raw / (1.0f + 0.02f * (currentTempC - 25.0f)));

    // Float switch — always read regardless of sleep state
    floatWaterDetected = (digitalRead(PIN_FLOAT_SWITCH) == LOW);

    // SLEEP: time window (18:00–08:00) takes priority; app sleep stacks on top.
    if (isSleeping || isInSleepHours())
    {
        digitalWrite(PIN_RELAY_PUMP_INTAKE, HIGH);
        digitalWrite(PIN_RELAY_PUMP_COLLECT, HIGH);
        digitalWrite(PIN_RELAY_PELTIER, HIGH);
        isIntakePumpOn = isCollectPumpOn = isPeltierOn = false;
        // Keep collect cycle reset so it fires promptly when the system wakes.
        collectCycleStart = now;
        return;
    }

    // --- PELTIER PRIORITY ---
    // Override from app takes precedence; otherwise follow the schedule.
    bool peltierWantsOn;
    if (overridePeltier == "on")
        peltierWantsOn = true;
    else if (overridePeltier == "off")
        peltierWantsOn = false;
    else
        peltierWantsOn = peltierShouldRun();

    if (peltierWantsOn)
    {
        // Peltier has exclusive relay access — force collection pump OFF.
        digitalWrite(PIN_RELAY_PUMP_COLLECT, HIGH);
        isCollectPumpOn = false;

        if (!floatWaterDetected)
        {
            // Basin empty: fill first, hold peltier off until water is detected.
            digitalWrite(PIN_RELAY_PUMP_INTAKE, LOW);
            isIntakePumpOn = true;
            digitalWrite(PIN_RELAY_PELTIER, HIGH);
            isPeltierOn = false;
        }
        else
        {
            // Basin has water: stop intake pump, run peltier.
            digitalWrite(PIN_RELAY_PUMP_INTAKE, HIGH);
            isIntakePumpOn = false;
            digitalWrite(PIN_RELAY_PELTIER, LOW);
            isPeltierOn = true;
        }

        // Reset collect cycle so it doesn't fire the instant peltier ends.
        collectCycleStart = now;
        return;
    }

    // --- NORMAL OPERATION ---
    isPeltierOn = false;
    digitalWrite(PIN_RELAY_PELTIER, HIGH);

    // Intake pump — float switch in auto; override from app.
    if (overrideIntakePump == "on")
    {
        digitalWrite(PIN_RELAY_PUMP_INTAKE, LOW);
        isIntakePumpOn = true;
    }
    else if (overrideIntakePump == "off")
    {
        digitalWrite(PIN_RELAY_PUMP_INTAKE, HIGH);
        isIntakePumpOn = false;
    }
    else
    {
        bool on = !floatWaterDetected;
        digitalWrite(PIN_RELAY_PUMP_INTAKE, on ? LOW : HIGH);
        isIntakePumpOn = on;
    }

    // Collection pump — 5 s ON every 30 min; override from app.
    unsigned long collectElapsed = now - collectCycleStart;
    if (collectElapsed >= COLLECT_CYCLE_MS)
        collectCycleStart = now;
    bool autoCollect = (collectElapsed < COLLECT_ON_MS);

    if (overrideCollectPump == "on")
    {
        digitalWrite(PIN_RELAY_PUMP_COLLECT, LOW);
        isCollectPumpOn = true;
    }
    else if (overrideCollectPump == "off")
    {
        digitalWrite(PIN_RELAY_PUMP_COLLECT, HIGH);
        isCollectPumpOn = false;
    }
    else
    {
        digitalWrite(PIN_RELAY_PUMP_COLLECT, autoCollect ? LOW : HIGH);
        isCollectPumpOn = autoCollect;
    }
}

// --- CLOUD SYNC ---
void syncWithProductionAPI()
{
    if (WiFi.status() != WL_CONNECTED)
        return;

    WiFiClientSecure *client = new WiFiClientSecure;
    if (!client)
        return;
    client->setInsecure();

    HTTPClient http;
    if (http.begin(*client, backend_url))
    {
        http.addHeader("Content-Type", "application/json");
        http.addHeader("X-API-Key", api_key);

        JsonDocument doc;
        doc["device_id"] = device_id;

        JsonObject sensorsObj = doc["sensors"].to<JsonObject>();
        sensorsObj["basin_temp"] = currentTempC;
        sensorsObj["tds_ppm"] = currentTds;
        sensorsObj["float_water_detect"] = floatWaterDetected;

        JsonObject actuatorsObj = doc["actuators"].to<JsonObject>();
        actuatorsObj["intake_pump_active"] = isIntakePumpOn;
        actuatorsObj["collect_pump_active"] = isCollectPumpOn;
        actuatorsObj["peltier_active"] = isPeltierOn;

        bool sleeping = isSleeping || isInSleepHours();

        // State priority: Sleeping → Heating → Filling (pre-heat) → Collecting → Refilling → Monitoring
        if (sleeping)
            doc["state"] = "Sleeping";
        else if (isPeltierOn)
            doc["state"] = "Heating";
        else if (peltierShouldRun() && !isPeltierOn)
            doc["state"] = "Filling";
        else if (isCollectPumpOn)
            doc["state"] = "Collecting";
        else if (isIntakePumpOn)
            doc["state"] = "Refilling";
        else
            doc["state"] = "Monitoring";

        String payload;
        serializeJson(doc, payload);
        Serial.println("[API] Syncing:");
        Serial.println(payload);

        int code = http.POST(payload);
        if (code == 201)
        {
            Serial.println("Success (201)");
        }
        else
        {
            Serial.printf("Error: %d\n", code);
            if (code == 401 || code == 403)
                Serial.println("WARNING: Check X-API-Key!");
        }

        // Parse override commands from app response.
        // Time-based sleep cannot be cancelled by the app.
        String responseBody = http.getString();
        Serial.println("[API] Raw response:");
        Serial.println(responseBody);

        JsonDocument resDoc;
        if (deserializeJson(resDoc, responseBody) == DeserializationError::Ok)
        {
            if (resDoc["sleep"].is<bool>())
                isSleeping = resDoc["sleep"].as<bool>();

            if (resDoc["commands"]["intake_pump_override"].is<const char *>())
                overrideIntakePump = resDoc["commands"]["intake_pump_override"].as<String>();
            if (resDoc["commands"]["collect_pump_override"].is<const char *>())
                overrideCollectPump = resDoc["commands"]["collect_pump_override"].as<String>();
            if (resDoc["commands"]["peltier_override"].is<const char *>())
                overridePeltier = resDoc["commands"]["peltier_override"].as<String>();

            Serial.printf("[CMD] app_sleep:%s  IN1:%s  IN2:%s  IN3:%s\n",
                          isSleeping ? "ON" : "OFF",
                          overrideIntakePump.c_str(),
                          overrideCollectPump.c_str(),
                          overridePeltier.c_str());
        }
        else
        {
            Serial.println("[CMD] Failed to parse response JSON");
        }

        http.end();
    }
    delete client;
}

void setup()
{
    pinMode(PIN_RELAY_PUMP_INTAKE, OUTPUT);
    digitalWrite(PIN_RELAY_PUMP_INTAKE, HIGH);

    pinMode(PIN_RELAY_PUMP_COLLECT, OUTPUT);
    digitalWrite(PIN_RELAY_PUMP_COLLECT, HIGH);

    pinMode(PIN_RELAY_PELTIER, OUTPUT);
    digitalWrite(PIN_RELAY_PELTIER, HIGH);

    pinMode(PIN_FLOAT_SWITCH, INPUT_PULLUP);

    Serial.begin(115200);
    sensors.begin();
    sensors.setWaitForConversion(false);

    WiFi.begin(ssid, password);
    Serial.print("Connecting WiFi");
    while (WiFi.status() != WL_CONNECTED)
    {
        delay(500);
        Serial.print(".");
    }
    Serial.println("\nWiFi connected.");

    // NTP — Philippines Standard Time (UTC+8, no DST)
    configTime(8 * 3600, 0, "pool.ntp.org", "time.nist.gov");
    Serial.print("Syncing NTP");
    struct tm timeinfo;
    while (!getLocalTime(&timeinfo))
    {
        delay(500);
        Serial.print(".");
    }
    Serial.printf("\nPST: %02d:%02d:%02d\n",
                  timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);

    collectCycleStart = millis();
    Serial.println("System Online.");
}

void loop()
{
    unsigned long now = millis();

    // DS18B20 temperature — non-blocking, every 5 s, data only
    static unsigned long lastTempRequest = 0;
    if (!tempRequested && (now - lastTempRequest >= TEMP_INTERVAL))
    {
        sensors.requestTemperatures();
        tempRequested = true;
        tempRequestTime = now;
        lastTempRequest = now;
    }
    if (tempRequested && (now - tempRequestTime >= TEMP_WAIT_MS))
    {
        float t = sensors.getTempCByIndex(0);
        Serial.printf("[DS18B20] %.1f C\n", t);
        if (t > -50 && t < 100)
            currentTempC = t;
        tempRequested = false;
    }

    // Fast sensors, actuators, API sync — every 500 ms
    static unsigned long lastFast = 0;
    if (now - lastFast >= FAST_INTERVAL)
    {
        lastFast = now;
        updateFastSensors();

        bool sleeping = isSleeping || isInSleepHours();

        // Collect pump cycle progress
        unsigned long collectElapsed = now - collectCycleStart;
        unsigned long collectRemaining = (collectElapsed < COLLECT_CYCLE_MS)
                                            ? (COLLECT_CYCLE_MS - collectElapsed) / 1000
                                            : 0;

        // Current local time
        struct tm timeinfo;
        char timeBuf[10] = "??:??:??";
        if (getLocalTime(&timeinfo))
            sprintf(timeBuf, "%02d:%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);

        Serial.println("─────────────────────────────────────────");
        Serial.printf(" TIME    : %s PST\n", timeBuf);
        Serial.printf(" WIFI    : %s\n", WiFi.status() == WL_CONNECTED ? "CONNECTED" : "DISCONNECTED");
        Serial.println(" --- SENSORS ---");
        Serial.printf(" TEMP    : %.1f C\n", currentTempC);
        Serial.printf(" TDS     : %d ppm  (raw ADC: %u)\n", currentTds, analogRead(PIN_TDS_ANALOG));
        Serial.printf(" FLOAT   : %s  (GPIO14 raw: %s)\n",
                      floatWaterDetected ? "WATER DETECTED" : "NO WATER",
                      digitalRead(PIN_FLOAT_SWITCH) == LOW ? "LOW" : "HIGH");
        Serial.println(" --- OVERRIDES (from app) ---");
        Serial.printf(" IN1 override : %s\n", overrideIntakePump.c_str());
        Serial.printf(" IN2 override : %s\n", overrideCollectPump.c_str());
        Serial.printf(" IN3 override : %s\n", overridePeltier.c_str());
        Serial.printf(" App sleep    : %s\n", isSleeping ? "YES" : "NO");
        Serial.println(" --- ACTUATORS ---");
        Serial.printf(" IN1 Intake pump  : %s\n", isIntakePumpOn  ? "ON" : "OFF");
        Serial.printf(" IN2 Collect pump : %s  (cycle: %lus elapsed, %lus until next)\n",
                      isCollectPumpOn ? "ON" : "OFF",
                      collectElapsed / 1000,
                      collectRemaining);
        Serial.printf(" IN3 Peltier      : %s\n", isPeltierOn ? "ON" : "OFF");
        Serial.println(" --- SYSTEM ---");
        Serial.printf(" SLEEP   : %s\n", sleeping ? "YES (relays OFF)" : "NO (operating)");
        Serial.printf(" PELTIER SCHEDULED: %s\n", peltierShouldRun() ? "YES" : "NO");
        Serial.println("─────────────────────────────────────────");

        syncWithProductionAPI();
    }
}
