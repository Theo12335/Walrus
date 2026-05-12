# Project Walrus — Complete Wiring Reference

## Pin Map Summary

| Component                        | ESP32 Pin | Mode                |
|----------------------------------|-----------|---------------------|
| DS18B20 (Temperature)            | GPIO 22   | Digital, OneWire    |
| TDS Sensor Signal                | GPIO 34   | Analog Input        |
| HC-SR04 TRIG — Clean water level | GPIO 19   | Digital Out         |
| HC-SR04 ECHO — Clean water level | GPIO 21   | Digital In          |
| Relay IN1 — Intake Pump          | GPIO 26   | Digital Out         |
| Relay IN2 — Collection Pump      | GPIO 27   | Digital Out         |
| Relay IN3 — PTC Heater (12V)     | GPIO 32   | Digital Out         |
| Relay IN4 — Atomizer/Mist        | GPIO 25   | Digital Out         |
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
    LOAD+ ──► Relay CH1, CH2, CH4 COM terminals (12V — pumps & heater)
    LOAD− ──► Common GND

[Buck Converter]
    IN+  ──► 12V Battery (+)
    IN−  ──► Common GND
    OUT+ ──► ESP32 VIN, Relay VCC, HC-SR04 VCC, Relay CH3 COM (5V — atomizer)
    OUT− ──► Common GND
```

> Set buck converter output to exactly **5V** before connecting.
> All GND connections share a single common ground rail.

---

## 2. DS18B20 Temperature Sensor

```
DS18B20 Pin     →  Connect To
─────────────────────────────────────────
VCC  (red)      →  ESP32 3.3V
GND  (black)    →  Common GND
DATA (yellow)   →  ESP32 GPIO 22

Pull-up: 4.7kΩ resistor between DATA and 3.3V (required)
```

---

## 3. TDS Sensor Module

```
TDS Module Pin  →  Connect To
─────────────────────────────────────────
VCC             →  ESP32 3.3V
GND             →  Common GND
AOUT / Signal   →  ESP32 GPIO 34
```

---

## 4. HC-SR04 — Clean Water Output Level

> ECHO outputs 5V — use voltage divider to protect ESP32 (max 3.3V input).

```
HC-SR04 Pin  →  Connect To
─────────────────────────────────────────
VCC          →  Buck converter 5V
GND          →  Common GND
TRIG         →  ESP32 GPIO 19
ECHO         →  Voltage divider (see below) → GPIO 21
```

### ECHO Voltage Divider (1kΩ + 2kΩ)

```
ECHO ──[ 1kΩ ]──┬── GPIO 21
                │
              [ 2kΩ ]
                │
               GND
```

Result: 5V × (2000 / 3000) = **3.33V** ✅

**Logic:** Distance ≤ 20cm = clean water present → collection pump ON.
Distance > 20cm = no water → collection pump OFF.

---

## 5. Float Switch (2-Wire Ball Float Switch)

```
Float Switch  →  Connect To
─────────────────────────────────────────
Wire 1        →  ESP32 GPIO 33
Wire 2        →  Common GND

(No external resistor — firmware uses INPUT_PULLUP)
```

**Logic:** LOW = water detected → intake pump OFF.
HIGH = no water → intake pump ON.

---

## 6. 4-Channel Relay Module (HW-316)

**Input side — connect once:**

```
Relay Module  →  Connect To
─────────────────────────────────────────
VCC           →  Buck converter 5V
GND           →  Common GND
IN1           →  ESP32 GPIO 26  (Intake Pump)
IN2           →  ESP32 GPIO 27  (Collection Pump)
IN3           →  ESP32 GPIO 32  (PTC Heater)
IN4           →  ESP32 GPIO 25  (Atomizer/Mist)
```

> Relay logic is **active-low**: IN LOW = ON, IN HIGH = OFF.
> All relays default HIGH (OFF) on boot.

### Channel 1 — Intake Pump

```
Relay 1 COM  →  Solar Controller LOAD+  (12V)
Relay 1 NO   →  Intake Pump (+) red wire
                 Intake Pump (−) black wire → LOAD− / Common GND
```

### Channel 2 — Collection Pump

```
Relay 2 COM  →  Solar Controller LOAD+  (12V)
Relay 2 NO   →  Collection Pump (+) red wire
                 Collection Pump (−) black wire → LOAD− / Common GND
```

### Channel 3 — PTC Heater (12V)

```
Relay 3 COM  →  Solar Controller LOAD+  (12V)
Relay 3 NO   →  PTC Heater (+) red wire
                 PTC Heater (−) black wire → LOAD− / Common GND
```

### Channel 4 — Atomizer/Mist Module

```
Relay 4 COM  →  Buck converter 5V
Relay 4 NO   →  Atomizer P1 (+)
                 Atomizer P1 (−) → Common GND
```

> PTC is a resistive load — **no flyback diode needed**.
> Heater turns ON below 25°C, OFF above 28°C (adjustable in code).

---

## 7. Atomizer Module (DC 5V — P1 wired)

```
Atomizer      →  Connect To
─────────────────────────────────────────
P1 (+)        →  Relay CH3 NO (switched 5V)
P1 (−)        →  Common GND
Disc output   →  Piezo disc (on-board)
```

> Do NOT connect to 12V — module is 5V only.
> Disc must be submerged in water before powering.

---

## 8. PTC Heating Element (12V)

```
PTC Heater    →  Connect To
─────────────────────────────────────────
(+) red       →  Relay CH4 NO
(−) black     →  Common GND / LOAD−
```

> No diode needed — purely resistive load.

---

## 9. Diodes

| Location | Diode | Status |
|---|---|---|
| Across relay coils | Built into HW-316 PCB | ✅ Already there |
| Across intake pump terminals | 1N4007 | ⚠️ Add this |
| Across collection pump terminals | 1N4007 | ⚠️ Add this |
| PTC heater | None needed | ✅ |
| Atomizer | None needed | ✅ |

---

## 10. Full System Block Diagram

```
[Solar Panel] ──► [Solar Charge Controller] ──► [12V Battery]
                          │
                     LOAD+ / LOAD−
                          │
               ┌──────────┴────────────────────────┐
               │       4-Channel Relay (HW-316)     │
               │   IN1    IN2    IN3    IN4          │
               └───┬──────┬──────┬──────┬───────────┘
               GPIO26  GPIO27  GPIO32  GPIO25
                   │      │      │      │
              [Intake] [Collect] │   [Heater]
               Pump    Pump    [Atomizer] 12V
                               5V Module

[12V BAT] ──► [Buck 12→5V]
                   ├──► ESP32 VIN
                   ├──► Relay VCC
                   ├──► HC-SR04 VCC
                   └──► Relay CH3 COM (atomizer 5V)

ESP32 3.3V ──► DS18B20 VCC + 4.7kΩ pull-up
           └──► TDS Module VCC

ESP32 GND  ──► Common GND rail (all components)
```

---

## 11. 3.3V vs 5V Reference

| Component | Powered By |
|---|---|
| DS18B20 | ESP32 3.3V |
| TDS Sensor | ESP32 3.3V |
| HC-SR04 | Buck converter 5V |
| Relay VCC | Buck converter 5V |
| Atomizer (via relay) | Buck converter 5V |
| ESP32 | Buck converter 5V (via VIN) |
| Intake Pump | Solar LOAD+ 12V (via relay) |
| Collection Pump | Solar LOAD+ 12V (via relay) |
| PTC Heater | Solar LOAD+ 12V (via relay) |

---

## 12. Quick-Reference: Troubleshooting

| Symptom | Check |
|---|---|
| DS18B20 reads -127°C | Missing 4.7kΩ pull-up resistor on DATA line |
| DS18B20 always 25.0 | Same as above — default value, sensor not reading |
| Ultrasonic always 999cm | Voltage divider wiring, sensor obstructed, or no 5V |
| Intake pump won't stop | Float switch wired wrong — one wire GPIO 33, other GND |
| Collection pump always OFF | Check HC-SR04 wiring and CLEAN_WATER_THRESHOLD (20cm) in code |
| Atomizer not running | Confirm 5V on Relay CH3 COM, wired to NO not NC, disc submerged |
| Heater not turning on | Confirm 12V on Relay CH4 COM, water temp reading correctly |
| Pumps not running | Confirm 12V on Relay CH1/CH2 COM from solar controller LOAD+ |
| ESP32 not booting | Check buck converter output is exactly 5V, min 500mA |
