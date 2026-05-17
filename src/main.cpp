/**
 * =============================================================================
 * PROJECT WALRUS - PRODUCTION API INTEGRATION (v12.0)
 * =============================================================================
 * Data Flow: ESP32 -> Vercel API (HTTPS) -> Supabase
 * Timezone: Philippines Standard Time (UTC+8, no DST)
 *
 * Operating window: 08:00–16:59 (PST)
 * Sleep window:     17:00–07:59 — all relays OFF, sensors still read
 *
 * Sensors (data only):
 *   DS18B20      — basin temperature
 *   TDS          — water quality
 *   Float switch — water presence in basin (NO type, closes to GND when water)
 *
 * Actuators:
 *   Intake pump  — ON when float switch detects no water, OFF when water present
 *   Collect pump — 5 s ON every 30 min (suspended while peltier is active)
 *   Peltier      — 10:30–14:30, 12 min ON / 18 min OFF cycle
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
const char *ssid = "Wassup2.4G";
const char *password = "Bascon12335";
const char *api_key = "walrus-esp32-key-2026";
const char *device_id = "WALRUS_001";
const char *backend_url = "https://walrus-pi.vercel.app/api/esp32/data";

// --- TIMING (fixed) ---
constexpr unsigned long TEMP_INTERVAL = 5000;
constexpr unsigned long TEMP_WAIT_MS = 800;

// --- RUNTIME CONFIG (overridden by app via POST response) ---
// Defaults match the original hardcoded constants. The app can change these
// live; firmware reads them from `config` in the POST response each sync.
unsigned long fastIntervalMs    = 500;
unsigned long collectOnMs       = 5000UL;
unsigned long collectCycleMs    = 30UL * 60UL * 1000UL;
int wakeMin                     = 8 * 60;       // 480 = 08:00
int sleepMin                    = 17 * 60;      // 1020 = 17:00
int peltierStartMin             = 10 * 60 + 30; // 630
int peltierStopMin              = 14 * 60 + 30; // 870
int peltierOnMin                = 12;
int peltierCycleMin             = 30;

// --- PIN DEFINITIONS ---
constexpr uint8_t PIN_DS18B20 = 22;
constexpr uint8_t PIN_TDS_ANALOG = 34;
constexpr uint8_t PIN_RELAY_PUMP_INTAKE = 26;  // IN1
constexpr uint8_t PIN_RELAY_PUMP_COLLECT = 27; // IN2
constexpr uint8_t PIN_RELAY_PELTIER = 32;      // IN3
constexpr uint8_t PIN_FLOAT_SWITCH = 33;

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

// True during the system sleep window (17:00–08:00 PST).
// Returns false if NTP has not synced yet (safe default: stay awake).
bool isInSleepHours()
{
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo))
        return false;
    int totalMin = timeinfo.tm_hour * 60 + timeinfo.tm_min;
    return (totalMin < wakeMin || totalMin >= sleepMin);
}

// True when the peltier should be in its ON phase (configurable window + cycle).
// Returns false outside the operating window or if NTP has not synced.
bool peltierShouldRun()
{
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo))
        return false;
    int totalMin = timeinfo.tm_hour * 60 + timeinfo.tm_min;
    if (totalMin < peltierStartMin || totalMin >= peltierStopMin)
        return false;
    int cycleMin = (totalMin - peltierStartMin) % peltierCycleMin;
    return cycleMin < peltierOnMin;
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

    // Float switch — NO type: LOW = switch closed = water present
    floatWaterDetected = (digitalRead(PIN_FLOAT_SWITCH) == LOW);

    // SLEEP: time window (17:00–08:00) takes priority; app sleep stacks on top.
    if (isSleeping || isInSleepHours())
    {
        digitalWrite(PIN_RELAY_PUMP_INTAKE, HIGH);
        digitalWrite(PIN_RELAY_PUMP_COLLECT, HIGH);
        digitalWrite(PIN_RELAY_PELTIER, HIGH);
        isIntakePumpOn = isCollectPumpOn = isPeltierOn = false;
        collectCycleStart = now;
        return;
    }

    // --- PELTIER PRIORITY ---
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
        // LOW = water present → pump OFF. HIGH = no water → pump ON.
        bool on = !floatWaterDetected;
        digitalWrite(PIN_RELAY_PUMP_INTAKE, on ? LOW : HIGH);
        isIntakePumpOn = on;
    }

    // Collection pump — configurable cycle/duration; override from app.
    unsigned long collectElapsed = now - collectCycleStart;
    if (collectElapsed >= collectCycleMs)
        collectCycleStart = now;
    bool autoCollect = (collectElapsed < collectOnMs);

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

            // Runtime config — schedule + durations. Defaults preserved if absent.
            JsonVariantConst cfg = resDoc["config"];
            if (cfg.is<JsonObject>())
            {
                if (cfg["wake_minute"].is<int>())              wakeMin         = cfg["wake_minute"].as<int>();
                if (cfg["sleep_minute"].is<int>())             sleepMin        = cfg["sleep_minute"].as<int>();
                if (cfg["peltier_start_minute"].is<int>())     peltierStartMin = cfg["peltier_start_minute"].as<int>();
                if (cfg["peltier_stop_minute"].is<int>())      peltierStopMin  = cfg["peltier_stop_minute"].as<int>();
                if (cfg["peltier_on_minutes"].is<int>())       peltierOnMin    = cfg["peltier_on_minutes"].as<int>();
                if (cfg["peltier_cycle_minutes"].is<int>())    peltierCycleMin = cfg["peltier_cycle_minutes"].as<int>();
                if (cfg["collect_cycle_minutes"].is<int>())    collectCycleMs  = (unsigned long)cfg["collect_cycle_minutes"].as<int>() * 60UL * 1000UL;
                if (cfg["collect_duration_seconds"].is<int>()) collectOnMs     = (unsigned long)cfg["collect_duration_seconds"].as<int>() * 1000UL;
                if (cfg["sync_interval_ms"].is<int>())         fastIntervalMs  = (unsigned long)cfg["sync_interval_ms"].as<int>();
            }

            Serial.printf("[CMD] app_sleep:%s  IN1:%s  IN2:%s  IN3:%s\n",
                          isSleeping ? "ON" : "OFF",
                          overrideIntakePump.c_str(),
                          overrideCollectPump.c_str(),
                          overridePeltier.c_str());
            Serial.printf("[CFG] wake:%d sleep:%d  pel:%d-%d (%d on / %d cycle)  collect:%lums/%lums  sync:%lums\n",
                          wakeMin, sleepMin, peltierStartMin, peltierStopMin,
                          peltierOnMin, peltierCycleMin,
                          collectOnMs, collectCycleMs, fastIntervalMs);
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

    // WiFi reconnect — non-blocking, retries every 10 s when disconnected
    static unsigned long lastWifiRetry = 0;
    if (WiFi.status() != WL_CONNECTED && now - lastWifiRetry >= 10000UL)
    {
        lastWifiRetry = now;
        WiFi.reconnect();
        Serial.println("[WIFI] Disconnected — attempting reconnect...");
    }

    // Fast sensors, actuators, API sync — interval is runtime-configurable
    static unsigned long lastFast = 0;
    if (now - lastFast >= fastIntervalMs)
    {
        lastFast = now;
        updateFastSensors();

        bool sleeping = isSleeping || isInSleepHours();

        unsigned long collectElapsed = now - collectCycleStart;
        unsigned long collectRemaining = (collectElapsed < collectCycleMs)
                                             ? (collectCycleMs - collectElapsed) / 1000
                                             : 0;

        struct tm timeinfo;
        char timeBuf[10] = "??:??:??";
        if (getLocalTime(&timeinfo))
            sprintf(timeBuf, "%02d:%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);

        Serial.println("─────────────────────────────────────────");
        Serial.printf(" TIME    : %s PST\n", timeBuf);
        Serial.printf(" WIFI    : %s\n", WiFi.status() == WL_CONNECTED ? "CONNECTED" : "DISCONNECTED");
        Serial.println(" --- SENSORS ---");
        Serial.printf(" TEMP    : %.1f C\n", currentTempC);
        Serial.printf(" TDS     : %d ppm\n", currentTds);
        Serial.printf(" FLOAT   : %s  (GPIO33 raw: %s)\n",
                      floatWaterDetected ? "WATER DETECTED" : "NO WATER",
                      digitalRead(PIN_FLOAT_SWITCH) == LOW ? "LOW" : "HIGH");
        Serial.println(" --- OVERRIDES (from app) ---");
        Serial.printf(" IN1 override : %s\n", overrideIntakePump.c_str());
        Serial.printf(" IN2 override : %s\n", overrideCollectPump.c_str());
        Serial.printf(" IN3 override : %s\n", overridePeltier.c_str());
        Serial.printf(" App sleep    : %s\n", isSleeping ? "YES" : "NO");
        Serial.println(" --- ACTUATORS (code vs raw GPIO) ---");
        Serial.printf(" IN1 Intake pump  : %s  (GPIO26: %s)\n",
                      isIntakePumpOn ? "ON" : "OFF",
                      digitalRead(PIN_RELAY_PUMP_INTAKE) == LOW ? "LOW=ON" : "HIGH=OFF");
        Serial.printf(" IN2 Collect pump : %s  (GPIO27: %s)  cycle: %lus elapsed, %lus until next\n",
                      isCollectPumpOn ? "ON" : "OFF",
                      digitalRead(PIN_RELAY_PUMP_COLLECT) == LOW ? "LOW=ON" : "HIGH=OFF",
                      collectElapsed / 1000,
                      collectRemaining);
        Serial.printf(" IN3 Peltier      : %s  (GPIO32: %s)\n",
                      isPeltierOn ? "ON" : "OFF",
                      digitalRead(PIN_RELAY_PELTIER) == LOW ? "LOW=ON" : "HIGH=OFF");
        Serial.println(" --- SYSTEM ---");
        Serial.printf(" SLEEP            : %s\n", sleeping ? "YES (relays OFF)" : "NO (operating)");
        Serial.printf(" PELTIER SCHEDULED: %s\n", peltierShouldRun() ? "YES" : "NO");
        Serial.println("─────────────────────────────────────────");

        syncWithProductionAPI();
    }
}
