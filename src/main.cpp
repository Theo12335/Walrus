/**
 * =============================================================================
 * PROJECT WALRUS - IoT Cloud Enabled (v3.0)
 * =============================================================================
 * - WiFi Connectivity (Local Hotspot/Pocket WiFi)
 * - Supabase Integration (JSON over HTTP POST)
 * - Ultrasonic Precision Refill
 * - TDS & Temp Monitoring
 * 
 * Target Hardware: ESP32-WROOM-32 (30-pin)
 * =============================================================================
 */

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <OneWire.h>
#include <DallasTemperature.h>

// --- WiFi & CLOUD CONFIGURATION ---
// Replace with your hotspot/pocket wifi credentials
const char* ssid     = "YOUR_WIFI_NAME"; 
const char* password = "YOUR_WIFI_PASS"; 

// Supabase REST API Configuration
// Format: https://[PROJECT_ID].supabase.co/rest/v1/[TABLE_NAME]
const char* supabase_url = "https://your-project-id.supabase.co/rest/v1/sensor_logs";
const char* supabase_key = "your-anon-key-here";

// --- PIN DEFINITIONS ---
constexpr uint8_t PIN_DS18B20    = 4;
constexpr uint8_t PIN_TDS_ANALOG = 34;
constexpr uint8_t PIN_ULTRA_TRIG = 19;
constexpr uint8_t PIN_ULTRA_ECHO = 18;
constexpr uint8_t PIN_RELAY_PUMP = 26;

// --- LOGIC & CALIBRATION ---
constexpr uint8_t RELAY_ON  = LOW;   // Active-LOW relay
constexpr uint8_t RELAY_OFF = HIGH;
constexpr float DIST_PUMP_ON_CM  = 7.5f; 
constexpr float DIST_PUMP_OFF_CM = 6.5f;

// --- TIMING ---
unsigned long lastSensorReadTime = 0;
unsigned long lastCloudSendTime  = 0;
const unsigned long SENSOR_INTERVAL_MS = 2000;   // Read sensors every 2s
const unsigned long CLOUD_INTERVAL_MS  = 10000;  // Send to Cloud every 10s

// --- GLOBAL OBJECTS ---
OneWire oneWire(PIN_DS18B20);
DallasTemperature sensors(&oneWire);

// --- GLOBAL STATE ---
float currentTempC = 25.0;
float currentDist  = 0.0;
int   currentTds   = 0;
bool  isPumpOn     = false;

// --- FUNCTION PROTOTYPES ---
void connectToWiFi();
void updateSensors();
void sendDataToSupabase();
float getDistance();

// =============================================================================
// SETUP
// =============================================================================
void setup() {
    // 1. Boot Safety (Relay OFF)
    pinMode(PIN_RELAY_PUMP, OUTPUT);
    digitalWrite(PIN_RELAY_PUMP, RELAY_OFF);
    
    Serial.begin(115200);
    delay(1000);
    Serial.println(F("\n--- PROJECT WALRUS: CLOUD MODE ---"));

    // 2. Hardware Init
    pinMode(PIN_ULTRA_TRIG, OUTPUT);
    pinMode(PIN_ULTRA_ECHO, INPUT);
    sensors.begin();
    analogReadResolution(12);
    analogSetAttenuation(ADC_11db);

    // 3. Connect to WiFi
    connectToWiFi();
}

// =============================================================================
// MAIN LOOP
// =============================================================================
void loop() {
    unsigned long now = millis();

    // Task 1: Sensor Readings & Local Control (2s)
    if (now - lastSensorReadTime >= SENSOR_INTERVAL_MS) {
        lastSensorReadTime = now;
        updateSensors();
        
        // Local Telemetry
        Serial.printf("T: %.1fC | D: %.1fcm | TDS: %d | Pump: %s\n", 
                      currentTempC, currentDist, currentTds, isPumpOn ? "ON" : "OFF");
    }

    // Task 2: Cloud Sync (10s)
    if (now - lastCloudSendTime >= CLOUD_INTERVAL_MS) {
        lastCloudSendTime = now;
        if (WiFi.status() == WL_CONNECTED) {
            sendDataToSupabase();
        } else {
            Serial.println(F("[WIFI] Disconnected! Reconnecting..."));
            WiFi.begin(ssid, password);
        }
    }
}

// =============================================================================
// CORE FUNCTIONS
// =============================================================================

void connectToWiFi() {
    Serial.print(F("[WIFI] Connecting to: "));
    Serial.println(ssid);
    
    WiFi.begin(ssid, password);
    
    // Timeout after 15 seconds if can't find hotspot
    unsigned long startAttemptTime = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - startAttemptTime < 15000) {
        delay(500);
        Serial.print(".");
    }

    if (WiFi.status() == WL_CONNECTED) {
        Serial.println(F("\n[WIFI] Connected!"));
        Serial.print(F("[WIFI] IP: "));
        Serial.println(WiFi.localIP());
    } else {
        Serial.println(F("\n[WIFI] Connection Failed (Proceeding in offline mode)"));
    }
}

float getDistance() {
    digitalWrite(PIN_ULTRA_TRIG, LOW); delayMicroseconds(2);
    digitalWrite(PIN_ULTRA_TRIG, HIGH); delayMicroseconds(10);
    digitalWrite(PIN_ULTRA_TRIG, LOW);
    
    long dur = pulseIn(PIN_ULTRA_ECHO, HIGH, 30000);
    if (dur == 0) return 999.0f; // Timeout/Error
    return (dur * 0.0343f) / 2.0f;
}

void updateSensors() {
    // 1. Temp (DS18B20)
    sensors.requestTemperatures();
    float t = sensors.getTempCByIndex(0);
    if (t > -50.0 && t < 100.0) currentTempC = t;
    
    // 2. Level (Ultrasonic)
    currentDist = getDistance();
    
    // 3. TDS (Analog)
    uint32_t analogVal = analogRead(PIN_TDS_ANALOG);
    float v = (analogVal / 4095.0) * 3.3;
    float raw = (133.42*pow(v,3) - 255.86*pow(v,2) + 857.39*v) * 0.5;
    currentTds = (int)(raw / (1.0 + 0.02*(currentTempC - 25.0)));

    // 4. Pump Control Logic
    if (currentDist > 50.0) { // Safety Off (Sensor error or too far)
        digitalWrite(PIN_RELAY_PUMP, RELAY_OFF);
        isPumpOn = false;
    } else if (currentDist >= DIST_PUMP_ON_CM) {
        digitalWrite(PIN_RELAY_PUMP, RELAY_ON);
        isPumpOn = true;
    } else if (currentDist <= DIST_PUMP_OFF_CM) {
        digitalWrite(PIN_RELAY_PUMP, RELAY_OFF);
        isPumpOn = false;
    }
}

void sendDataToSupabase() {
    HTTPClient http;
    
    // Initialize JSON document
    JsonDocument doc;
    doc["temperature"] = currentTempC;
    doc["tds"]         = currentTds;
    doc["water_level"] = currentDist;
    doc["pump_status"] = isPumpOn ? "ON" : "OFF";
    
    String jsonPayload;
    serializeJson(doc, jsonPayload);

    // Prepare HTTP Request
    http.begin(supabase_url);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("apikey", supabase_key);
    http.addHeader("Authorization", String("Bearer ") + supabase_key);
    http.addHeader("Prefer", "return=minimal"); // Save bandwidth

    Serial.print(F("[CLOUD] Sending data to Supabase... "));
    int httpResponseCode = http.POST(jsonPayload);

    if (httpResponseCode > 0) {
        Serial.print(F("Success! Code: "));
        Serial.println(httpResponseCode);
    } else {
        Serial.print(F("Failed. Error: "));
        Serial.println(http.errorToString(httpResponseCode).c_str());
    }

    http.end();
}
