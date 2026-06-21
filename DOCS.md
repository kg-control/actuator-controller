# Actuator Controller — Documentation

> **License:** [CC BY-NC 4.0](https://creativecommons.org/licenses/by-nc/4.0/) © 2026 Mackenzie Glenfadden.
> Free for personal/non-commercial use. Commercial licensing: kenzieglen@gmail.com

---

## Table of Contents

1. [Overview](#overview)
2. [Bill of Materials](#bill-of-materials)
3. [Hardware Assembly & Wiring](#hardware-assembly--wiring)
4. [Firmware Setup](#firmware-setup)
5. [First Boot & Wi-Fi Configuration](#first-boot--wi-fi-configuration)
6. [Web UI](#web-ui)
7. [Telegram Bot](#telegram-bot)
8. [HTTP API Reference](#http-api-reference)
9. [How It Works](#how-it-works)

---

## Overview

This project turns a linear actuator into a Wi-Fi-controlled device using an ESP32
microcontroller, a DRV8871 motor driver, and a 24 V DC power supply. Once assembled,
the actuator can be controlled from any browser on your local network, or remotely via a
Telegram bot.

The actuator is reciprocating — it converts rotational motor movement to linear push/pull
motion, so it behaves identically in either direction. The firmware drives it with a
single PWM channel; the DRV8871's second input is tied to ground.

---

## Bill of Materials

| Component | Notes |
|---|---|
| ESP32 development board (ESP-WROOM-32) | Any standard 30-pin DevKit |
| DRV8871 motor driver breakout | SparkFun or equivalent |
| 24 V linear actuator | Must match your power supply voltage |
| 24 V DC power supply (wall adapter) | Current rating depends on your actuator |
| 5 V buck converter (step-down module, marked "4R7") | Powers the ESP32 from the 24 V rail |
| Jumper wires | |
| Breadboard or perfboard | |

---

## Hardware Assembly & Wiring

### Overview

The project was designed using a 30-pin DevKit. Other options may be used, but the `GPIO18` 
pin referenced in step 4 below might need to be adjusted accordingly. Review your equipment
before wiring.

The 24 V wall supply powers both the actuator (via the DRV8871) and the ESP32 (via a 5 V
buck converter stepped down from the same 24 V rail). The ESP32 sends a PWM signal to the
DRV8871, which switches the full 24 V to the actuator.

### Wiring Table

| From | To | Wire colour (diagram) |
|---|---|---|
| 24 V supply `+` | DRV8871 `VM` | Red |
| 24 V supply `+` | Buck converter `IN+` | Red |
| 24 V supply `−` | DRV8871 `GND` | Black |
| 24 V supply `−` | Buck converter `IN−` | Black |
| Buck converter `OUT+` | ESP32 `VIN` | Red |
| Buck converter `OUT−` | ESP32 `GND` | Black |
| ESP32 `GPIO18` (D18) | DRV8871 `IN1` | Yellow |
| ESP32 `GND` | DRV8871 `IN2` | Black (tied to GND) |
| DRV8871 `OUT1` | Actuator black wire | Green |
| DRV8871 `OUT2` | Actuator red wire | Blue |

### Step-by-step

1. **Set the buck converter output to 5 V** before connecting it to the ESP32. Use a
   multimeter between `OUT+` and `OUT−` while powered from the 24 V supply, and adjust the
   trimmer until you read 5.0 V.

2. **Wire the power rails.** Connect the 24 V supply positive to both the DRV8871 `VM`
   terminal and the buck converter `IN+`. Connect the negative to DRV8871 `GND` and buck
   `IN−`.

3. **Wire the ESP32 power.** Connect buck `OUT+` to ESP32 `VIN` and buck `OUT−` to ESP32
   `GND`.

4. **Wire the control signal.** Run a wire from ESP32 `GPIO18` to DRV8871 `IN1`. Tie
   DRV8871 `IN2` permanently to ground (ESP32 GND or the supply negative).

5. **Wire the actuator.** Connect DRV8871 `OUT1` to the actuator's black wire and `OUT2`
   to the actuator's red wire.

6. **Double-check polarity** before applying power. Reversed polarity on the DRV8871 `VM`
   pin will damage the driver.

> **Safety note:** The 24 V rail is live whenever the wall supply is plugged in, even if
> the ESP32 is unplugged. Always unplug the wall adapter before re-wiring.

---

## Firmware Setup

### Prerequisites

- [Arduino IDE](https://www.arduino.cc/en/software) 2.x, or PlatformIO
- ESP32 board package installed
  (Arduino IDE → Boards Manager → search "esp32" by Espressif)

### Required Libraries

Install all of the following via Arduino IDE's Library Manager
(Sketch → Include Library → Manage Libraries):

| Library | Author |
|---|---|
| `WiFiManager` | tzapu |
| `ArduinoJson` | Benoit Blanchon |
| `UniversalTelegramBot` | Brian Lough |

`WiFi`, `WebServer`, and `Preferences` are part of the ESP32 Arduino core and need no
separate installation.

### Flashing

1. Open `actuator_controller.ino` in Arduino IDE.
2. Under **Tools → Board**, select **ESP32 Dev Module** (or your specific variant).
3. Set **Tools → Upload Speed** to `115200`.
4. Connect the ESP32 via USB, select the correct **Port**, and click **Upload**.
5. Open the **Serial Monitor** (115200 baud) to watch boot messages.

---

## First Boot & Wi-Fi Configuration

On first boot (or any time saved Wi-Fi credentials are lost), the ESP32 enters
**configuration mode**:

1. It creates an access point named **`Actuator-Setup`**.
2. Connect to that network from your phone or laptop.
3. A captive portal will open automatically (or navigate to `192.168.4.1`).
4. Fill in:
   - **Wi-Fi SSID & password** — your home/workshop network.
   - **Telegram Bot Token** — leave blank if you do not want Telegram control.
5. Click **Save**. The ESP32 will reboot and connect to your network.
6. Check the Serial Monitor for the assigned IP address:
   ```
   Connected! IP: 192.168.1.42
   ```

Credentials are stored in non-volatile flash (via `Preferences`) and survive reboots and
power cuts.

### Updating Credentials

To re-enter configuration mode, erase the ESP32's flash
(Arduino IDE → Tools → Erase All Flash Before Sketch Upload → Enabled) and re-flash, or
call `WiFiManager::resetSettings()` programmatically.

---

## Web UI

Open a browser and navigate to the ESP32's IP address (e.g. `http://192.168.1.42`).

The interface has two inputs and two buttons:

| Control | Description |
|---|---|
| **Speed (1–100 %)** | Motor drive level. Lower values are slower; 60 % is a sensible starting point. |
| **Duration (seconds)** | How long to run before auto-stopping. |
| **Run** | Starts the actuator at the chosen speed for the chosen duration. |
| **Stop** | Stops the actuator immediately, regardless of remaining time. |

The status box at the bottom polls `/status` every 2 seconds and shows the current state,
speed, and seconds remaining.

---

## Telegram Bot

Telegram control lets you start, stop, and query the actuator from anywhere with an
internet connection.

### Creating a Bot

1. Open Telegram and search for **@BotFather**.
2. Send `/newbot` and follow the prompts to choose a name and username.
3. BotFather will give you a token in the format `123456789:AAF...`. Copy it.
4. Enter this token in the Wi-Fi configuration portal (see [First Boot](#first-boot--wi-fi-configuration)).

### Commands

| Command | Example | Description |
|---|---|---|
| `/run [speed] [duration]` | `/run 80 30` | Run at 80 % speed for 30 seconds. Defaults: speed 80, duration 30. |
| `/stop` | `/stop` | Stop immediately. |
| `/status` | `/status` | Report current state, speed, and time remaining. |
| `/help` | `/help` | List all commands. |

> **Note:** The bot polls Telegram every 1 second. There may be up to a ~1 s delay
> between sending a command and the actuator responding.

> **Security:** There is no user authentication — anyone who knows your bot's username can
> send commands. For private use, keep the bot token secret and consider using Telegram's
> privacy settings to disable adding the bot to groups.

---

## HTTP API Reference

All endpoints use `HTTP GET` and return JSON. The base URL is the ESP32's IP address.

### `GET /`

Returns the web UI (HTML page). No parameters.

---

### `GET /run`

Starts the actuator.

**Query parameters:**

| Parameter | Type | Range | Required | Description |
|---|---|---|---|---|
| `speed` | integer | 1–100 | Yes | Motor speed as a percentage. |
| `duration` | integer | ≥ 1 | Yes | Run time in seconds. |

**Success response (`200`):**
```json
{
  "status": "running",
  "speed": 80,
  "duration": 30
}
```

**Error response (`400`):**
```json
{ "error": "Required params: speed (1-100), duration (seconds)" }
```

**Example:**
```
GET http://192.168.1.42/run?speed=80&duration=30
```

---

### `GET /stop`

Stops the actuator immediately.

**Response (`200`):**
```json
{ "status": "stopped" }
```

---

### `GET /status`

Returns the current actuator state.

**Response when running (`200`):**
```json
{
  "status": "running",
  "speed": 80,
  "time_remaining": 17
}
```

**Response when stopped (`200`):**
```json
{ "status": "stopped" }
```

---

## How It Works

### PWM Motor Control

The ESP32 uses its built-in `ledcWrite` API to output a PWM signal on GPIO18 at 12 kHz
with 8-bit resolution (0–255). The duty cycle is mapped from the user's 1–100 % speed
value to a hardware range of 153–255. The lower bound of the duty cycle map (153 at 8-bit, 
76 at 7-bit) represents the minimum drive level at which the actuator will move under load 
rather than stall. This value varies between actuators and should be determined by testing. 
First, temporarily set the map to use the full range:

```cpp
int duty = map(percent, 1, 100, 1, 255);  // or 1, 127 at 7-bit
```

Run the actuator starting at 1% speed and increase gradually until you find the lowest 
percentage that produces reliable movement. Then multiply that percentage (as a decimal) 
by your resolution ceiling to get the lower bound — for example, if the actuator starts 
moving at 60% with 8-bit resolution: 0.60 × 255 = 153. Substitute that result as the 
third argument:

```cpp
int duty = map(percent, 1, 100, 153, 255);  // calibrated lower bound
```

```
Speed (%)  →  PWM duty (0–255)
1 %        →  153  (minimum movement threshold)
100 %      →  255  (full drive)
```

**Tuning for audible whine:** If the motor driver emits a high-pitched whine during operation, 
it is resonating at the PWM switching frequency. The fix is to push PWM_FREQ above the threshold 
of human hearing (~20 kHz). A value of 20000–22000 is recommended — high enough to be inaudible, 
without unnecessary switching losses in the DRV8871. Note that frequency and resolution are coupled 
through the ESP32's timer: at higher frequencies the maximum achievable resolution decreases. 
If you raise PWM_FREQ above ~20 kHz, drop PWM_RESOLUTION from 8 to 7 and update the duty cycle map 
accordingly — the upper bound drops from 255 to 127:

```cpp
#define PWM_FREQ       20000
#define PWM_RESOLUTION 7

// In motorSetSpeed():
int duty = map(percent, 1, 100, 76, 127);  // calibrated lower bound -- see above
```

Avoid going significantly higher than 22 kHz — it increases driver heat and EMI without any audible benefit.

### Auto-Stop Timer

When a run command is issued, the firmware records `millis() + (duration × 1000)` as the
stop timestamp. The main `loop()` checks this on every iteration and calls `motorStop()`
once the timestamp is reached. This is non-blocking — the web server and Telegram polling
continue to function during a run.

### Persistence

The Telegram bot token is stored in ESP32 flash using the `Preferences` library (a
key-value store backed by NVS). Wi-Fi credentials are managed by WiFiManager, which uses
its own NVS partition.
