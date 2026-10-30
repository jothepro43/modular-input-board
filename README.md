# Modular Control Panel — prototype firmware

> **Status:** this README describes the older direct-wired prototype. The current firmware is `firmware/modular_remote`. See [HANDOFF.md](HANDOFF.md) for where the project stands.

Two Arduino sketches for the ENGR 1101 prototype.

| Sketch | Board | What it does |
|---|---|---|
| `firmware/qtpy_media_remote` | Adafruit QT Py ESP32-S3 | The real prototype. Pairs with a phone or laptop over Bluetooth and controls whatever is playing to your speaker. |
| `firmware/esp8266_hardware_test` | ESP-12E DevKit V2 (ESP8266) | Wiring test you can run today. Same knob, keys and display, but no Bluetooth, so it only shows results on the display and in the Serial Monitor. |

## Controls

| Control | Tap | Hold |
|---|---|---|
| Knob (turn) | Volume up / down, one step per click | — |
| Knob (press) | Mute / unmute | 1.5 s: **sync** — turns the phone all the way down and resets the display to 0 |
| Play key | Play / pause | — |
| Skip key | Next track | 0.7 s: previous track |

The display shows the volume as a number, `OFF` when muted, `-bt-` blinking while waiting to pair, and a word (`PLAY`, `SKIP`, `PrEV`, `SYnC`) for about a second after each action.

**About the volume number:** Bluetooth media keys can only say "volume up" or "volume down". The phone never sends its real volume back, so the number is the panel's estimate. If someone changes the volume on the phone itself, the number drifts. Hold the knob for 1.5 seconds to sync it again.

## Wiring — QT Py ESP32-S3 (breadboard)

Use male-to-female jumpers on the encoder and display pins. **Power everything from the 3V pin, never 5V.** The QT Py's pins are 3.3 V.

| Part | Part pin | QT Py pin |
|---|---|---|
| KY-040 encoder | `+` | 3V |
| | `GND` | GND |
| | `CLK` | A0 |
| | `DT` | A1 |
| | `SW` | A2 |
| Play switch | one leg | A3 |
| | other leg | GND |
| Skip switch | one leg | TX |
| | other leg | GND |
| TM1637 display | `VCC` | 3V |
| | `GND` | GND |
| | `CLK` | SCK |
| | `DIO` | MOSI |

SDA and SCL stay free for the I2C modules later. To use different pins, change the `PIN_...` lines at the top of the sketch.

## Wiring — ESP-12E DevKit V2 (test sketch)

| Part | Part pin | DevKit pin |
|---|---|---|
| KY-040 | `+` / `GND` | 3V3 / GND |
| | `CLK` / `DT` / `SW` | D5 / D6 / D7 |
| Play switch | legs | D1 and GND |
| Skip switch | legs | D2 and GND |
| TM1637 | `VCC` / `GND` | 3V3 / GND |
| | `CLK` / `DIO` | D3 / D4 |

## Setup in the Arduino IDE

1. **Board packages** (Tools → Board → Boards Manager):
   - QT Py: install **esp32 by Espressif Systems**. Select **Adafruit QT Py ESP32-S3 No PSRAM**, and set **USB CDC On Boot: Enabled** so the Serial Monitor works.
   - ESP-12E: add `https://arduino.esp8266.com/stable/package_esp8266com_index.json` under File → Preferences → Additional boards manager URLs, install **esp8266**, and select **NodeMCU 1.0 (ESP-12E Module)**.
2. **Libraries** (Sketch → Include Library → Manage Libraries):
   - **TM1637** by Avishay Orpaz (both sketches)
   - **ESP32 BLE Keyboard** by T-vK (QT Py sketch only)
3. Plug in with a **data** USB cable, pick the port, and click Upload.
4. Open the Serial Monitor at **115200** baud to see each action printed.

## Pair and test (QT Py)

1. After uploading, the display blinks `-bt-`.
2. On the phone: Settings → Bluetooth → pair with **Modular Panel**.
3. Play a song to the speaker from the phone.
4. Hold the knob 1.5 s to sync, then turn it. The phone volume and the display should move together.

## Troubleshooting

| Problem | Fix |
|---|---|
| Computer doesn't see the board | Try another cable. Many USB cables are charge-only. The ESP-12E may also need the CH340 USB driver on Windows or macOS. |
| `BleKeyboard` won't compile, with errors mentioning `std::string` and `String` | A known mismatch between that library and newer ESP32 board packages. The common fix is to install **esp32 version 2.0.17** in Boards Manager. |
| Knob turns the wrong way | Set `ENC_REVERSE = true`. |
| One click changes the volume twice, or it takes two clicks per step | Change `ENC_STEPS_PER_DETENT` to `2`. |
| Pressing the knob does random things | The SW pin needs its pull-up. The sketch sets `INPUT_PULLUP`; check SW goes to A2. |
| Display is dim or blank | Raise `DISPLAY_BRIGHTNESS` (0–7). Check VCC is on 3V and CLK/DIO aren't swapped. |
| Phone pairs but nothing happens | Unpair, restart the board, and pair again. Some phones need the media app open in the foreground. |

## How the code is organized

- **Encoder:** interrupts on both pins feed a quadrature table, which ignores contact bounce and counts one step per detent.
- **Buttons:** a small `Button` struct debounces each key and reports a tap (released early) or a hold (fires once while still held).
- **Display:** redraws only when something changes, so it doesn't flicker.
- **Bluetooth:** `BleKeyboard` sends standard media keys, which phones, tablets and computers already understand. Nothing to install on the phone.

Checked on a desktop compiler with all warnings on, and the input logic was simulated: knob clicks, contact bounce, taps, holds and short blips. It has not been run on real hardware yet, so expect to adjust `ENC_STEPS_PER_DETENT` or `ENC_REVERSE` on the first test.
