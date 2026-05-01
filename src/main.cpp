/**
 * =============================================================================
 * PROJECT WALRUS - PRODUCTION API INTEGRATION (v6.0)
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
const char *ssid = "YONG";
const char *password = "GOODSHIT";
const char *api_key = "walrus-esp32-key-2026"; // <-- Must match backend ESP32_API_KEY
const char *device_id = "WALRUS_001";
const char *backend_url = "https://walrus-pi.vercel.app/api/esp32/data";

// --- PIN DEFINITIONS ---
constexpr uint8_t PIN_DS18B20 = 4;
constexpr uint8_t PIN_TDS_ANALOG = 34;
constexpr uint8_t PIN_ULTRA_TRIG = 19;
constexpr uint8_t PIN_ULTRA_ECHO = 18;
constexpr uint8_t PIN_RELAY_PUMP = 26;

// --- GLOBAL STATE ---
float currentTempC = 25.0;
float currentDist = 0.0;
int currentTds = 0;
bool isPumpOn = false;
unsigned long lastCloudSync = 0;

OneWire oneWire(PIN_DS18B20);
DallasTemperature sensors(&oneWire);

// --- SENSOR LOGIC ---
float getDistance()
{
    digitalWrite(PIN_ULTRA_TRIG, LOW);
    delayMicroseconds(2);
    digitalWrite(PIN_ULTRA_TRIG, HIGH);
    delayMicroseconds(10);
    digitalWrite(PIN_ULTRA_TRIG, LOW);
    long dur = pulseIn(PIN_ULTRA_ECHO, HIGH, 30000);
    return (dur == 0) ? 999.0f : (dur * 0.0343f) / 2.0f;
}

void updateSensors()
{
    sensors.requestTemperatures();
    float t = sensors.getTempCByIndex(0);
    if (t > -50 && t < 100)
        currentTempC = t;

    currentDist = getDistance();

    uint32_t analogVal = analogRead(PIN_TDS_ANALOG);
    float v = (analogVal / 4095.0) * 3.3;
    float raw = (133.42 * pow(v, 3) - 255.86 * pow(v, 2) + 857.39 * v) * 0.5;
    currentTds = (int)(raw / (1.0 + 0.02 * (currentTempC - 25.0)));

    // Pump Logic
    if (currentDist > 50.0)
    {
        digitalWrite(PIN_RELAY_PUMP, HIGH);
        isPumpOn = false;
    }
    else if (currentDist >= 7.5)
    {
        digitalWrite(PIN_RELAY_PUMP, LOW);
        isPumpOn = true;
    }
    else if (currentDist <= 6.5)
    {
        digitalWrite(PIN_RELAY_PUMP, HIGH);
        isPumpOn = false;
    }
}

// --- CLOUD SYNC ---
void syncWithProductionAPI()
{
    if (WiFi.status() != WL_CONNECTED)
        return;

    WiFiClientSecure *client = new WiFiClientSecure;
    if (client)
    {
        client->setInsecure();

        HTTPClient http;
        if (http.begin(*client, backend_url))
        {
            http.addHeader("Content-Type", "application/json");
            http.addHeader("X-API-Key", api_key);

            // Construct Production JSON Body
            JsonDocument doc;
            doc["device_id"] = device_id;

            JsonObject sensorsObj = doc["sensors"].to<JsonObject>();
            sensorsObj["basin_temp"] = currentTempC;
            sensorsObj["tds_ppm"] = currentTds;
            sensorsObj["water_level_cm"] = currentDist;

            JsonObject actuatorsObj = doc["actuators"].to<JsonObject>();
            actuatorsObj["pump_active"] = isPumpOn;

            // Determine System State
            if (currentDist > 50.0)
            {
                doc["state"] = "Idle";
            }
            else if (isPumpOn)
            {
                doc["state"] = "Refilling";
            }
            String payload;
            serializeJson(doc, payload);

            Serial.println("[API] Syncing to Vercel Production:");
            Serial.println(payload); // Print the actual JSON payload

            int httpResponseCode = http.POST(payload);

            if (httpResponseCode == 201)
            {
                Serial.println("Success (201 Created)");
            }
            else
            {
                Serial.printf("Error Code: %d\n", httpResponseCode);
                if (httpResponseCode == 401 || httpResponseCode == 403)
                {
                    Serial.println("WARNING: Check your X-API-Key!");
                }
            }
            http.end();
        }
        delete client;
    }
}

void setup()
{
    pinMode(PIN_RELAY_PUMP, OUTPUT);
    digitalWrite(PIN_RELAY_PUMP, HIGH);

    Serial.begin(115200);
    pinMode(PIN_ULTRA_TRIG, OUTPUT);
    pinMode(PIN_ULTRA_ECHO, INPUT);
    sensors.begin();

    WiFi.begin(ssid, password);
    Serial.print("Connecting to Internet");
    while (WiFi.status() != WL_CONNECTED)
    {
        delay(500);
        Serial.print(".");
    }
    Serial.println("\nSystem Online.");
}

void loop()
{
    static unsigned long lastLocal = 0;
    if (millis() - lastLocal >= 2000)
    {
        lastLocal = millis();
        updateSensors();
        Serial.printf("T:%.1f D:%.1f TDS:%d P:%s\n", currentTempC, currentDist, currentTds, isPumpOn ? "ON" : "OFF");
    }

    if (millis() - lastCloudSync >= 15000)
    {
        lastCloudSync = millis();
        syncWithProductionAPI();
    }
}
