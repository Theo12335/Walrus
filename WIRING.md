# Project Walrus — Complete Wiring Reference

## Pin Map Summary

| Component                        | ESP32 Pin | Mode                |
|----------------------------------|-----------|---------------------|
| DS18B20 (Temperature)            | GPIO 4    | Digital, OneWire    |
| TDS Sensor Signal                | GPIO 34   | Analog Input        |
| HC-SR04 TRIG — Clean water level | GPIO 19   | Digital Out         |
| HC-SR04 ECHO — Clean water level | GPIO 18   | Digital In          |
| Relay IN1 — Intake Pump          | GPIO 26   | Digital Out         |
| Relay IN2 — Collection Pump      | GPIO 27   | Digital Out         |
| Relay IN3 — Atomizer/Mist        | GPIO 32   | Digital Out         |
| Float Switch                     | GPIO 33   | Digital In (PULLUP) |

---

## 1. Power System

```
[Solar Panel]
    (+) ──► Solar Charge Controller  PV+
    (−) ──► Solar Charge Controller  PV−

[12V Battery]
    (+) ──► Solar Charge Controller  BAT+
    (−) ──► Solar Charge Controller  BAT−

[Solar Charge Controller]
    LOAD+ ──► 4-Channel Relay COM terminals (12V for pumps)
    LOAD− ──► Common GND

    USB 5V out ──► ESP32 USB port
    (or DC-DC buck converter: 12V BAT → 5V → ESP32 VIN)
```

> All GND connections (ESP32, sensors, relay module) share a common ground.

---

## 2. DS18B20 Temperature Sensor

```
DS18B20 Pin     →  Connect To
─────────────────────────────────────────
VCC  (red)      →  ESP32 3.3V
GND  (black)    →  ESP32 GND
DATA (yellow)   →  ESP32 GPIO 4

Pull-up: 4.7kΩ resistor between DATA and 3.3V (required)
```

---

## 3. TDS Sensor Module

```
TDS Module Pin  →  Connect To
─────────────────────────────────────────
VCC             →  ESP32 3.3V
GND             →  ESP32 GND
AOUT / Signal   →  ESP32 GPIO 34
```

---

## 4. HC-SR04 — Clean Water Output Level

> ECHO outputs 5V — use voltage divider to protect ESP32 (max 3.3V).

```
HC-SR04 Pin  →  Connect To
─────────────────────────────────────────
VCC          →  ESP32 5V (VIN)
GND          →  ESP32 GND
TRIG         →  ESP32 GPIO 19
ECHO         →  Voltage divider (see below) → GPIO 18
```

### ECHO Voltage Divider (5V → 3.3V)

```
ECHO ──[ 1kΩ ]──┬── GPIO 18
                │
              [ 2kΩ ]
                │
               GND
```

**Logic:** Distance ≤ 20cm = clean water present → collection pump ON.
Distance > 20cm = no water → collection pump OFF.

---

## 6. Float Switch (2-Wire Ball Float Switch)

```
Float Switch  →  Connect To
─────────────────────────────────────────
Wire 1        →  ESP32 GPIO 33
Wire 2        →  ESP32 GND

(No external resistor — firmware uses INPUT_PULLUP)
```

**Logic:** LOW = water detected → intake pump OFF.
HIGH = no water → intake pump ON.

---

## 7. 4-Channel Relay Module (HW-316)

Connect shared power pins once, then each channel independently:

```
Relay Module  →  Connect To
─────────────────────────────────────────
VCC           →  ESP32 5V (VIN)
GND           →  ESP32 GND
IN1           →  ESP32 GPIO 26  (Intake Pump)
IN2           →  ESP32 GPIO 27  (Collection Pump)
IN3           →  ESP32 GPIO 32  (Atomizer/Mist)
IN4           →  (spare)
```

> Relay logic is **active-low**: IN_ LOW = ON, IN_ HIGH = OFF.
> All relays default HIGH (OFF) on boot.

### Channel 1 — Intake Pump (draws water into basin)

```
Relay 1 COM  →  Solar Controller LOAD+  (12V)
Relay 1 NO   →  Intake Pump (+) red wire
                 Intake Pump (−) black wire → LOAD−
```

### Channel 2 — Collection Pump (moves clean water to storage)

```
Relay 2 COM  →  Solar Controller LOAD+  (12V)
Relay 2 NO   →  Collection Pump (+) red wire
                 Collection Pump (−) black wire → LOAD−
```

### Channel 3 — Atomizer/Mist Module

```
Relay 3 COM  →  5V supply (buck converter or USB power)
Relay 3 NO   →  Atomizer module VCC (red wire)
                 Atomizer module GND → Common GND
```

---

## 8. Atomizer Module (DC 5V Mist Transducer with Driver Board)

```
Atomizer Wire  →  Connect To
─────────────────────────────────────────
Red  (VCC)     →  Relay 3 NO terminal (switched 5V)
Black (GND)    →  Common GND
```

> Do NOT power directly from ESP32 3.3V — use the relay-switched 5V line.

---

## 9. Sleep / Wake Behaviour

The ESP32 enters **deep sleep** automatically:
- At **8:00 PM** — checked every loop cycle via NTP time
- On **boot** if time is between 8PM and 6AM
- When the **mobile app sends** `{"sleep": true}` in the API response

It wakes up at **6:00 AM** via the RTC timer.
All relays (pumps, mist) are turned OFF before sleeping.

---

## 10. Full System Block Diagram

```
[Solar Panel] ──► [Solar Charge Controller] ──► [12V Battery]
                         │
                    LOAD+ / LOAD−
                         │
              ┌──────────┴──────────────────┐
              │     4-Channel Relay (HW-316) │
              │  IN1   IN2   IN3   IN4(spare)│
              └──┬─────┬─────┬──────────────┘
            GPIO26  GPIO27  GPIO32
                │      │      │
          [Intake]  [Collect] [Atomizer]
           Pump      Pump      5V Module

[12V BAT] ──► [Buck 12→5V] ──► ESP32 VIN
                            ├──► Relay VCC
                            └──► HC-SR04 VCC

ESP32 3.3V ──► DS18B20 VCC + 4.7kΩ pull-up
           └──► TDS Module VCC

ESP32 GND  ──► All component GNDs (common ground)
```

---

## Quick-Reference: Troubleshooting

| Symptom | Check |
|---|---|
| DS18B20 reads -127°C | Missing 4.7kΩ pull-up resistor on DATA line |
| Ultrasonic always 999cm | ECHO voltage divider missing, or sensor obstructed |
| Intake pump won't stop | Float switch wired wrong — confirm one wire to GPIO 33, other to GND |
| Collection pump always OFF | Check HC-SR04 #2 wiring and CLEAN_WATER_THRESHOLD value in code |
| Mist module not running | Confirm 5V through Relay 3 NO terminal, not NC |
| ESP32 won't wake from sleep | Check NTP sync — if time is wrong it may sleep indefinitely |
| ESP32 not booting | Check 5V supply — needs stable 5V / 500mA minimum |
