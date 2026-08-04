# ESP32 Combined Sensor Dashboard

An ESP32 sketch that combines an **SH1106 OLED**, **BME280** (temperature/humidity/pressure), **MPU6050** (accelerometer/gyro), **MQ7** (analog CO gas sensor), and a **DS3231 RTC** into one button-navigated dashboard — using a **TCA9548A I2C multiplexer** to keep every I2C device electrically isolated on its own channel.



## Features

- **4 OLED pages** — Time, Environment, IMU, Gas — advanced manually with a push button (no auto-cycling)
- **Page indicator dots** at the bottom of the screen show which page is active
- **Heartbeat indicator** (top-right corner) blinks on every screen refresh, on every page, so you can confirm the sketch is alive at a glance
- **Debounced button input**, polled every loop with no blocking `delay()`
- All readings are also printed to **Serial (115200 baud)** every refresh, regardless of which page is showing
- Each sensor initializes and fails independently — one missing/broken sensor doesn't take down the others
- One-time RTC fallback to compile-time if the DS3231 has never been set or lost backup power

## Why a multiplexer?

None of these devices strictly address-conflict on their own (OLED `0x3C`, BME280 `0x76`/`0x77`, MPU6050 `0x68`/`0x69`) — **except** the DS3231, which also defaults to `0x68`, the same as MPU6050's primary address. Putting each device on its own TCA9548A channel sidesteps that collision and also electrically isolates each sensor, which helps with bus lockups, cross-talk, or a single misbehaving device (e.g. a slow-clock-stretching BME280 clone) glitching the others.

## Hardware Required

| Component | Notes |
|---|---|
| ESP32 dev board | Any variant with I2C + ADC1 |
| TCA9548A | I2C multiplexer, 8 channels (4 used here) |
| SH1106 OLED (128×64) | I2C, e.g. common 1.3" modules |
| BME280 | Temp / humidity / pressure, I2C |
| MPU6050 | Accel / gyro, I2C (GY-87 10-DOF combo boards work fine — only the MPU6050 portion is used) |
| MQ7 | Analog CO gas sensor |
| DS3231 | RTC module, I2C, with coin-cell backup battery |
| Push button | Momentary, normally-open |

## Wiring

**ESP32 → TCA9548A (the only device on the ESP32's direct I2C bus):**

| ESP32 | TCA9548A |
|---|---|
| GPIO21 (SDA) | SDA |
| GPIO22 (SCL) | SCL |
| 3.3V | VIN |
| GND | GND |

Default TCA9548A address is `0x70` — leave A0/A1/A2 unconnected unless you need to change it.

**TCA9548A channels → downstream devices:**

| Mux Channel | Device | Power |
|---|---|---|
| CH0 (SD0/SC0) | OLED | 3.3V / GND |
| CH1 (SD1/SC1) | BME280 | 3.3V / GND |
| CH2 (SD2/SC2) | MPU6050 | 3.3V / GND |
| CH3 (SD3/SC3) | DS3231 | 3.3V / GND |

**MQ7 (analog — bypasses the mux/I2C bus entirely):**

| MQ7 | ESP32 |
|---|---|
| VCC | 3.3V |
| GND | GND |
| AOUT | GPIO34 (ADC1 — ADC2 is unreliable once WiFi is active) |

**Button:**

| Button | ESP32 |
|---|---|
| Leg 1 | GPIO27 |
| Leg 2 | GND |

Uses the ESP32's internal pull-up (`INPUT_PULLUP`) — no external resistor needed. The pin reads LOW while pressed.

> **Note:** I2C runs at 100kHz instead of the 400kHz default, for signal-integrity reasons found while bringing the OLED up on a breadboard. Keep this unless you've confirmed clean wiring at 400kHz.

## Dependencies

Install via **Arduino IDE → Tools → Manage Libraries**:

- Adafruit GFX Library
- Adafruit SH110X
- Adafruit BME280 Library
- Adafruit MPU6050
- Adafruit Unified Sensor *(shared dependency)*
- RTClib *(by Adafruit — for the DS3231)*

No library is needed for the TCA9548A itself — it's controlled with a couple of raw `Wire` calls in the sketch.

## Setup

1. Wire everything per the [Wiring](#wiring) section above.
2. Install the [dependencies](#dependencies) listed above.
3. Open the sketch in the Arduino IDE, select your ESP32 board, and upload.
4. On first boot, the OLED will show "Initializing..." then "MQ7 warming up..." for a few seconds.
5. Press the button connected to GPIO27 to cycle through pages: **Time → Environment → IMU → Gas → (back to Time)**.

> The 3-second warmup on boot only avoids a completely bogus first MQ7 reading — for real accuracy, let the MQ7 warm up for several minutes before trusting its output.

## Configuration

Key constants near the top of the sketch you may want to adjust:

| Constant | Default | Purpose |
|---|---|---|
| `I2C_ADDRESS` | `0x3C` | OLED address (try `0x3D` if not found) |
| `TCA9548A_ADDRESS` | `0x70` | Mux address |
| `MQ7_PIN` | `34` | Must stay on ADC1 |
| `REFRESH_INTERVAL_MS` | `500` | Screen/serial refresh rate |
| `DEBOUNCE_MS` | `50` | Button debounce window |

## RTC Time Note

If the DS3231 has never been set, or its backup battery died and it lost power, the sketch sets it **once** to the time it was compiled. This is a one-time fallback, not a real time sync — if you need accurate time, sync it properly (e.g. via NTP over WiFi, or a dedicated "set time" sketch) rather than relying on the compile-time stamp.

## Troubleshooting

OLED is blank
- Try `I2C_ADDRESS` `0x3D` instead of `0x3C`.
- Run an I2C scanner to confirm devices are detected at all before touching code.
</details>

OLED shows static/snow
- Almost always I2C signal integrity on a breadboard — loose jumpers, long wires, or too fast a clock. Reseat wires and keep SDA/SCL runs short.

"BME280 not found!"
- Double-check SDA/SCL wiring and that VCC is 3.3V (most boards aren't 5V tolerant).
- Run an I2C scanner — it should report `0x76` or `0x77`.
- Some clone "BME280" boards are mislabeled BMP280 (no humidity sensor) — humidity readings will be garbage on those.

"MPU6050 not found!"
- Double-check SDA/SCL wiring to the MPU6050 specifically.
- On a GY-87 10-DOF board, only the MPU6050 portion (`0x68`/`0x69`) is used — the magnetometer/barometer are ignored.


MQ7 readings look stuck or nonsensical
MQ7 needs several minutes of warmup for accurate readings — the 3-second boot delay just avoids a totally bogus first reading.
- Confirm AOUT is wired to GPIO34 (ADC1) — ADC2 pins are unreliable once WiFi is active.


Button does nothing / erratic page changes
- Confirm the button shorts GPIO27 to GND when pressed (this sketch uses `INPUT_PULLUP`).
- On 4-leg tactile buttons, make sure you're using two legs that are actually bridged internally (usually diagonal pairs) — test with a multimeter if unsure.
- Erratic double-jumps are almost always a wiring/contact issue rather than code — debouncing is already handled (`DEBOUNCE_MS = 50`).


Nothing works / "SH1106 not found" despite correct wiring
- Confirm the TCA9548A itself shows up: run an I2C scanner against the ESP32's direct bus (before selecting any channel) and check for `0x70`.
- Confirm each sensor is on the channel the sketch expects: OLED → CH0, BME280 → CH1, MPU6050 → CH2, DS3231 → CH3. Swapped channel wiring is the most common mistake.


"DS3231 not found!" / wrong time on the Time page
- Confirm it's on channel 3 (SD3/SC3) and VCC/GND are wired.
- The DS3231 defaults to `0x68`, the same as MPU6050's primary address — this is exactly why each is on its own mux channel.
- A wrong-but-plausible time usually means the RTC lost backup power and fell back to compile time, which drifts further every time you re-flash. Replace the coin cell or sync via NTP.


