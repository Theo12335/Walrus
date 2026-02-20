/**
 * =============================================================================
 * PROJECT WALRUS - Automated Solar Still Desalination Controller
 * =============================================================================
 *
 * Target Hardware: ESP32-WROOM-32 (30-pin Dev Module)
 * Framework: Arduino (PlatformIO)
 *
 * System Overview:
 * ----------------
 * This firmware controls an automated solar still desalination system.
 * It monitors water temperature, TDS (Total Dissolved Solids), and basin
 * water level. An automated refill pump maintains optimal water levels
 * in the evaporation basin.
 *
 * Hardware Connections:
 * ---------------------
 * - DS18B20 Temperature Sensor  -> GPIO 4  (4.7kΩ pull-up to 3.3V)
 * - TDS Sensor (Analog)         -> GPIO 34 (ADC1_CH6, Input Only)
 * - Ultrasonic Trigger          -> GPIO 19
 * - Ultrasonic Echo             -> GPIO 18
 * - Relay IN1 (Refill Pump)     -> GPIO 26 (Active-LOW)
 *
 * Author: Project WALRUS Team
 * Version: 1.0.0
 * =============================================================================
 */

#include <Arduino.h>
#include <OneWire.h>
#include <DallasTemperature.h>

// =============================================================================
// PIN DEFINITIONS
// =============================================================================
constexpr uint8_t PIN_DS18B20       = 4;    // OneWire data pin for temperature sensor
constexpr uint8_t PIN_TDS_ANALOG    = 34;   // ADC input for TDS sensor (ADC1_CH6)
constexpr uint8_t PIN_ULTRA_TRIG    = 19;   // Ultrasonic trigger (TX)
constexpr uint8_t PIN_ULTRA_ECHO    = 18;   // Ultrasonic echo (RX)
constexpr uint8_t PIN_RELAY_PUMP    = 26;   // Relay IN1 - Refill pump control

// =============================================================================
// SYSTEM CONFIGURATION
// =============================================================================

// Timing Configuration (all values in milliseconds)
constexpr unsigned long SENSOR_READ_INTERVAL_MS = 2000;  // Read sensors every 2 seconds
constexpr unsigned long ULTRASONIC_TIMEOUT_US   = 30000; // 30ms timeout (~5m max range)

// Water Level Thresholds (in centimeters)
// Note: Higher distance = lower water level (sensor mounted above basin)
constexpr float TANK_EMPTY_CM = 15.0f;  // Start refilling when distance >= 15cm
constexpr float TANK_FULL_CM  = 5.0f;   // Stop refilling when distance <= 5cm

// ADC Configuration
constexpr float ADC_RESOLUTION   = 4095.0f;  // 12-bit ADC (0-4095)
constexpr float ADC_REF_VOLTAGE  = 3.3f;     // ESP32 ADC reference voltage

// TDS Calculation Constants
// Based on standard TDS probe calibration (adjust TDS_FACTOR if needed)
constexpr float TDS_FACTOR       = 0.5f;     // Conversion factor: voltage to ppm
constexpr float TEMP_COEFFICIENT = 0.02f;    // Temperature compensation coefficient
constexpr float TEMP_REFERENCE   = 25.0f;    // Reference temperature for TDS (°C)

// Sensor Validity Thresholds
constexpr float TEMP_SENSOR_ERROR    = -127.0f;  // DS18B20 returns this on error
constexpr float TEMP_MIN_VALID       = -10.0f;   // Minimum plausible temperature
constexpr float TEMP_MAX_VALID       = 100.0f;   // Maximum plausible temperature
constexpr float DISTANCE_MIN_VALID   = 2.0f;     // Minimum valid distance (cm)
constexpr float DISTANCE_MAX_VALID   = 400.0f;   // Maximum valid distance (cm)

// Relay Logic (Active-LOW relay module)
constexpr uint8_t RELAY_ON  = LOW;   // Drive LOW to activate relay
constexpr uint8_t RELAY_OFF = HIGH;  // Drive HIGH to deactivate relay

// =============================================================================
// GLOBAL OBJECTS & STATE VARIABLES
// =============================================================================

// OneWire bus and temperature sensor
OneWire oneWire(PIN_DS18B20);
DallasTemperature tempSensor(&oneWire);

// Timing state
unsigned long lastSensorReadTime = 0;

// Current sensor readings (global for cross-sensor calculations)
float currentTempC      = 0.0f;
float currentTdsPpm     = 0.0f;
float currentDistanceCm = 0.0f;
bool  pumpIsRunning     = false;

// Sensor health flags
bool tempSensorValid      = false;
bool ultrasonicSensorValid = false;

// =============================================================================
// FUNCTION PROTOTYPES
// =============================================================================
void initializeHardware();
void readAllSensors();
float readTemperature();
float readTdsSensor(float waterTempC);
float readUltrasonicDistance();
void updatePumpControl();
void printTelemetry();

// =============================================================================
// SETUP
// =============================================================================
void setup() {
    // -------------------------------------------------------------------------
    // CRITICAL: Initialize relay pin to OFF state BEFORE setting pinMode
    // This prevents the pump from activating during ESP32 boot sequence,
    // which could cause uncontrolled flooding of the basin.
    // -------------------------------------------------------------------------
    digitalWrite(PIN_RELAY_PUMP, RELAY_OFF);
    pinMode(PIN_RELAY_PUMP, OUTPUT);
    digitalWrite(PIN_RELAY_PUMP, RELAY_OFF);  // Ensure OFF after pinMode

    // Initialize Serial communication
    Serial.begin(115200);
    while (!Serial && millis() < 3000) {
        // Wait up to 3 seconds for Serial connection (non-blocking)
    }

    Serial.println();
    Serial.println(F("==========================================="));
    Serial.println(F("  PROJECT WALRUS - Solar Still Controller"));
    Serial.println(F("  Firmware v1.0.0"));
    Serial.println(F("==========================================="));
    Serial.println();

    // Initialize all hardware peripherals
    initializeHardware();

    Serial.println(F("[SYSTEM] Initialization complete. Starting main loop..."));
    Serial.println();
}

// =============================================================================
// MAIN LOOP (Non-blocking)
// =============================================================================
void loop() {
    unsigned long currentTime = millis();

    // Execute sensor reading and control logic at defined interval
    if (currentTime - lastSensorReadTime >= SENSOR_READ_INTERVAL_MS) {
        lastSensorReadTime = currentTime;

        // Read all sensors
        readAllSensors();

        // Update pump control based on water level
        updatePumpControl();

        // Output telemetry to Serial Monitor
        printTelemetry();
    }

    // Additional non-blocking tasks can be added here
    // (e.g., watchdog feed, communication handling, etc.)
}

// =============================================================================
// HARDWARE INITIALIZATION
// =============================================================================
void initializeHardware() {
    Serial.println(F("[INIT] Configuring GPIO pins..."));

    // Configure ultrasonic sensor pins
    pinMode(PIN_ULTRA_TRIG, OUTPUT);
    pinMode(PIN_ULTRA_ECHO, INPUT);
    digitalWrite(PIN_ULTRA_TRIG, LOW);

    // TDS pin is input-only (GPIO 34), no explicit pinMode needed for ADC
    // but we'll set it for clarity
    pinMode(PIN_TDS_ANALOG, INPUT);

    // Configure ADC for better accuracy
    // ADC1 (GPIOs 32-39) - using 12-bit resolution, 11dB attenuation for 0-3.3V range
    analogReadResolution(12);
    analogSetAttenuation(ADC_11db);  // Full 0-3.3V range

    Serial.println(F("[INIT] Initializing DS18B20 temperature sensor..."));
    tempSensor.begin();

    // Check if sensor is connected
    int deviceCount = tempSensor.getDeviceCount();
    if (deviceCount > 0) {
        Serial.print(F("[INIT] Found "));
        Serial.print(deviceCount);
        Serial.println(F(" DS18B20 sensor(s)"));

        // Set resolution to 12-bit for maximum accuracy (750ms conversion time)
        // For faster readings, use 9-bit (94ms) or 10-bit (188ms)
        tempSensor.setResolution(12);
        tempSensor.setWaitForConversion(false);  // Non-blocking mode
    } else {
        Serial.println(F("[WARNING] No DS18B20 sensor detected!"));
    }

    Serial.println(F("[INIT] Hardware initialization complete"));
    Serial.print(F("[INIT] Pump relay initialized to: "));
    Serial.println(pumpIsRunning ? "ON" : "OFF");
}

// =============================================================================
// SENSOR READING FUNCTIONS
// =============================================================================

/**
 * Read all sensors and update global state variables.
 * Sensor readings are performed in sequence with proper error handling.
 */
void readAllSensors() {
    // Read temperature first (needed for TDS compensation)
    currentTempC = readTemperature();

    // Read TDS with temperature compensation
    currentTdsPpm = readTdsSensor(currentTempC);

    // Read ultrasonic distance
    currentDistanceCm = readUltrasonicDistance();
}

/**
 * Read temperature from DS18B20 sensor.
 *
 * @return Temperature in Celsius, or TEMP_SENSOR_ERROR on failure
 */
float readTemperature() {
    // Request temperature conversion from all sensors on the bus
    tempSensor.requestTemperatures();

    // Small delay for conversion (non-blocking alternative would use async mode)
    // For 12-bit resolution, conversion takes ~750ms, but we're reading cached value
    // from previous request in a 2-second loop, so this is acceptable

    // Read temperature from first sensor (index 0)
    float tempC = tempSensor.getTempCByIndex(0);

    // Validate reading
    if (tempC == DEVICE_DISCONNECTED_C || tempC <= TEMP_SENSOR_ERROR) {
        tempSensorValid = false;
        return TEMP_SENSOR_ERROR;
    }

    // Additional sanity check
    if (tempC < TEMP_MIN_VALID || tempC > TEMP_MAX_VALID) {
        tempSensorValid = false;
        return TEMP_SENSOR_ERROR;
    }

    tempSensorValid = true;
    return tempC;
}

/**
 * Read and calculate TDS value with temperature compensation.
 *
 * The TDS calculation process:
 * 1. Read raw ADC value (0-4095 for 12-bit)
 * 2. Convert to voltage (0-3.3V)
 * 3. Convert voltage to raw TDS using probe calibration factor
 * 4. Apply temperature compensation formula
 *
 * @param waterTempC Current water temperature for compensation
 * @return Compensated TDS value in ppm
 */
float readTdsSensor(float waterTempC) {
    // Take multiple readings and average for stability
    const int NUM_SAMPLES = 10;
    long adcSum = 0;

    for (int i = 0; i < NUM_SAMPLES; i++) {
        adcSum += analogRead(PIN_TDS_ANALOG);
    }

    float avgAdc = (float)adcSum / NUM_SAMPLES;

    // Convert ADC value to voltage
    float voltage = (avgAdc / ADC_RESOLUTION) * ADC_REF_VOLTAGE;

    // Convert voltage to raw TDS (ppm)
    // Standard formula: TDS = (133.42 * V^3 - 255.86 * V^2 + 857.39 * V) * 0.5
    // Simplified linear approximation for typical TDS probes:
    float rawTds = (133.42f * voltage * voltage * voltage
                  - 255.86f * voltage * voltage
                  + 857.39f * voltage) * TDS_FACTOR;

    // Ensure non-negative
    if (rawTds < 0.0f) {
        rawTds = 0.0f;
    }

    // Apply temperature compensation
    // Formula: compensatedTDS = rawTDS / (1.0 + 0.02 * (temp - 25.0))
    float tempCompensation;

    if (tempSensorValid && waterTempC > TEMP_MIN_VALID) {
        // Use actual water temperature for compensation
        tempCompensation = 1.0f + TEMP_COEFFICIENT * (waterTempC - TEMP_REFERENCE);
    } else {
        // Fallback: assume 25°C if temperature sensor is invalid
        tempCompensation = 1.0f;
    }

    // Prevent division by zero or negative compensation
    if (tempCompensation <= 0.0f) {
        tempCompensation = 1.0f;
    }

    float compensatedTds = rawTds / tempCompensation;

    return compensatedTds;
}

/**
 * Read distance from ultrasonic sensor (JSN-SR04T or A02YYUW).
 *
 * Uses standard trigger/echo timing method:
 * 1. Send 10µs trigger pulse
 * 2. Measure echo pulse duration
 * 3. Calculate distance: distance = (duration * speed_of_sound) / 2
 *
 * @return Distance in centimeters, or 0 on timeout/error
 */
float readUltrasonicDistance() {
    // Ensure trigger pin is LOW before starting
    digitalWrite(PIN_ULTRA_TRIG, LOW);
    delayMicroseconds(2);

    // Send 10µs trigger pulse
    digitalWrite(PIN_ULTRA_TRIG, HIGH);
    delayMicroseconds(10);
    digitalWrite(PIN_ULTRA_TRIG, LOW);

    // Measure echo pulse duration with timeout
    unsigned long duration = pulseIn(PIN_ULTRA_ECHO, HIGH, ULTRASONIC_TIMEOUT_US);

    // Check for timeout (pulseIn returns 0 on timeout)
    if (duration == 0) {
        ultrasonicSensorValid = false;
        return 0.0f;
    }

    // Calculate distance in centimeters
    // Speed of sound = 343 m/s = 0.0343 cm/µs
    // Distance = (duration * 0.0343) / 2 = duration / 58.2
    float distanceCm = (float)duration / 58.2f;

    // Validate reading
    if (distanceCm < DISTANCE_MIN_VALID || distanceCm > DISTANCE_MAX_VALID) {
        ultrasonicSensorValid = false;
        return 0.0f;
    }

    ultrasonicSensorValid = true;
    return distanceCm;
}

// =============================================================================
// PUMP CONTROL LOGIC
// =============================================================================

/**
 * Update refill pump state based on water level with hysteresis.
 *
 * Control Logic:
 * - Turn ON pump when distance >= TANK_EMPTY_CM (water level low)
 * - Turn OFF pump when distance <= TANK_FULL_CM (water level high)
 * - Maintain current state when distance is between thresholds
 *
 * Safety Override:
 * - Force pump OFF if ultrasonic sensor returns invalid data (0 or timeout)
 *   This prevents overflow if the sensor is disconnected or malfunctioning.
 */
void updatePumpControl() {
    // -------------------------------------------------------------------------
    // SAFETY OVERRIDE: Check for sensor failure
    // -------------------------------------------------------------------------
    // If ultrasonic sensor returns 0 (timeout) or invalid reading,
    // force pump OFF to prevent basin overflow
    if (!ultrasonicSensorValid || currentDistanceCm <= 0.0f) {
        if (pumpIsRunning) {
            digitalWrite(PIN_RELAY_PUMP, RELAY_OFF);
            pumpIsRunning = false;
            Serial.println(F("[SAFETY] Pump forced OFF - Ultrasonic sensor error"));
        }
        return;
    }

    // -------------------------------------------------------------------------
    // NORMAL OPERATION: Hysteresis-based pump control
    // -------------------------------------------------------------------------

    // Water level LOW (distance high) - Start filling
    if (currentDistanceCm >= TANK_EMPTY_CM) {
        if (!pumpIsRunning) {
            digitalWrite(PIN_RELAY_PUMP, RELAY_ON);
            pumpIsRunning = true;
            Serial.println(F("[PUMP] Started - Tank low, refilling..."));
        }
    }
    // Water level HIGH (distance low) - Stop filling
    else if (currentDistanceCm <= TANK_FULL_CM) {
        if (pumpIsRunning) {
            digitalWrite(PIN_RELAY_PUMP, RELAY_OFF);
            pumpIsRunning = false;
            Serial.println(F("[PUMP] Stopped - Tank full"));
        }
    }
    // Distance between thresholds - maintain current state (hysteresis)
    // No action needed, pump stays in its current state
}

// =============================================================================
// TELEMETRY OUTPUT
// =============================================================================

/**
 * Print formatted sensor data to Serial Monitor.
 * Format: Temp: XX.X C | TDS: XXX ppm | Dist: XX cm | Pump: [ON/OFF]
 */
void printTelemetry() {
    Serial.print(F("Temp: "));

    if (tempSensorValid) {
        if (currentTempC < 10.0f && currentTempC >= 0.0f) {
            Serial.print(F(" "));  // Padding for single-digit temps
        }
        Serial.print(currentTempC, 1);
    } else {
        Serial.print(F("ERR "));
    }
    Serial.print(F(" C | "));

    Serial.print(F("TDS: "));
    if (currentTdsPpm < 10.0f) {
        Serial.print(F("  "));
    } else if (currentTdsPpm < 100.0f) {
        Serial.print(F(" "));
    }
    Serial.print((int)currentTdsPpm);
    Serial.print(F(" ppm | "));

    Serial.print(F("Dist: "));
    if (ultrasonicSensorValid) {
        if (currentDistanceCm < 10.0f) {
            Serial.print(F(" "));
        }
        Serial.print((int)currentDistanceCm);
    } else {
        Serial.print(F("ERR"));
    }
    Serial.print(F(" cm | "));

    Serial.print(F("Pump: "));
    Serial.println(pumpIsRunning ? F("[ON]") : F("[OFF]"));
}
