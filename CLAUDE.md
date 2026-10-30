# Modular Input Board — handoff for Claude Code

ENGR 1101 group project at UNG. Joseph Oprea is the hardware/firmware lead.
Team name on reports: "Three Acres And a Cow Prospects".
Phase (as of 2026-10-30): **prototyping**. Enclosure and firmware are NOT final.
**The Adafruit parts have arrived.** Next step: bring up the modular I2C firmware on the QT Py.

## Read this first: status as of 2026-10-30
See `HANDOFF.md`. It overrides this file where they differ:
- The panel uses **Apple Media Service, not a BLE keyboard**. Current firmware: `firmware/modular_remote`.
- OLED is at **0x3D**, not 0x3C. The STEMMA QT port is confirmed as `Wire1`. Encoder button pin 24 is confirmed.
- Development is on a **Mac** with Homebrew arduino-cli, not the Windows PC.
- The menu opens on a **3 s** hold.

## Electrical decisions confirmed 2026-10-30
- PCA9548 **Vlogic switch = 3V**. The mux ports output 3.3 V power + logic, so **slots carry
  3.3 V, not 5 V** (older notes that say 5V on the slot are wrong). The 3V regulator is good for 500 mA.
- NeoKey socket **V+ → ATtiny817 3V pin** (not Vin), so the NeoPixel data level matches 3.3 V logic.
  NeoKey GND → ATtiny GND. Switch: one pin → ATtiny input, other → GND. NeoPixel data-in → an
  ATtiny817 seesaw NeoPixel-capable pin (**pick it from the Adafruit guide**).

## Toolchain on Joseph's PC
- Arduino IDE 2.3.10 bundles arduino-cli, likely at
  `C:\Users\Joseph\AppData\Local\Programs\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe`
  (**verify**). Use it to compile, upload and monitor from the terminal instead of clicking in the IDE.
- Needed for the QT Py: the `esp32` core by Espressif (use **2.0.17** if ESP32 BLE Keyboard is used)
  and the libraries Adafruit seesaw, Adafruit SSD1306, Adafruit GFX and Adafruit NeoPixel. Consider
  NimBLE-based HID if ESP32 BLE Keyboard gives trouble.

## What the product is

A book-sized panel of snap-in tactile modules (knobs, keys, screen). First function: a
**Bluetooth media remote for a speaker**:

- Volume knob: turn = volume up/down, press = mute, hold 1.5 s = "sync" (sends 25× volume-down, resets estimate to 0)
- Play/Pause key
- Skip key: tap = next track, hold 0.7 s = previous track
- Screen: volume estimate, `OFF` when muted, short words (PLAY, SKIP, PrEV, SYnC), pairing status

**Architecture decision:** the panel is a *remote*, not in the audio path. Phone → speaker
stays as-is (BT or AUX). Panel pairs with the phone as a **BLE HID keyboard sending media
keys**. Phones accept a BLE keyboard and a BT speaker at the same time.

Known limit: HID media volume is relative only, so the displayed volume is an estimate. Phones
have ~15–16 volume steps, so finer encoder resolution doesn't give finer phone volume.

## Hardware

### Brain
- **Adafruit QT Py ESP32-S3, 8 MB flash / no PSRAM (#5426)**: BLE 5 + native USB-C. ESP32-S3
  has BLE only (no Bluetooth Classic/A2DP), which is fine because we only send key presses.
- Test board Joseph owns: **Geekcreit ESP-12E DevKit V2 (ESP8266)**: Wi-Fi only, **no
  Bluetooth**. Used only to test knob + display wiring on a breadboard.
- Alternative considered: Faketec boards Joseph owns (nRF52840 Pro Micro clone). Has BLE HID
  and low power, but needs a firmware rewrite (Adafruit nRF52 core) and has no STEMMA QT.
  Not chosen yet. QT Py is the plan.

### Final modular architecture
- Every slot carries only **GND, 5V, SDA, SCL** (I2C). Every module must speak I2C.
- **PCA9548 I2C mux (#5626)**: one mux channel per slot, so identical modules don't clash and
  firmware knows which slot each module is in. 8 channels per board. Prototype needs 1; full
  13-slot panel needs 2.
- Slot connector: **Adafruit DIY Magnetic Connector, 4-pin right angle (#5358)**, sold as a
  male+female pair. **Reversed halves repel**, so 4-pin is safe without the mirrored 6-pin
  layout or a keyed cartridge. Female half in the slot floor, male half in the module.
- Slot wiring: cut a 200 mm STEMMA QT cable (#4401) in half. One half goes slot side (plug into
  the mux, cut end to the female connector), the other goes module side (plug into the module
  board, cut end to the male connector). No backplane PCB.
- Modules:
  - Knob: **I2C rotary encoder w/ encoder (#5880)** (seesaw) + slim rubber D-shaft knob (#5093)
  - Keys (×2): Keychron brown MX switch (owned) in a **NeoKey socket breakout (#4978)**, wired by
    hand to an **ATtiny817 seesaw breakout (#5233)**. NeoKey = holder + NeoPixel, ATtiny = I2C.
    Keycaps: relegendable 10-pack (#5039), with paper icons under the clear cover.
  - Screen: **0.96" 128×64 OLED STEMMA QT (#326)**, SSD1306. Fits one 40 mm slot (29×27 mm), so
    the screen is a movable module. Firmware should scan mux channels for it. If no screen is
    found, show pairing status on the QT Py's onboard NeoPixel.
- Rejected / removed: NeoPixel ring around knob (removed from plan), routing audio through the
  panel, 1.3" OLED (#938, too big), M2.5 heat-set inserts (screw M2.5 straight into ~2.2 mm
  printed holes instead).
- Optional later: battery module (LiPo #258 + PowerBoost 1000C #2465, out of stock), with its own
  22 AWG power wire, not through the slot connector. Smoother knob options: Bourns PEC11R
  detent-free on breakout #4991, or AS5600 magnetic angle sensor (#6357). Software acceleration
  is the cheap first fix.

### Prototype breadboard (ESP-12E, parts owned)
| Part | Pin | ESP-12E |
|---|---|---|
| KY-040 | CLK / DT / SW | D5 / D6 / D7 |
| KY-040 | + / GND | 3V3 / GND |
| TM1637 | CLK / DIO | D3 / D4 |
| TM1637 | VCC / GND | 3V3 / GND |
| Play / Skip switch (optional, not wired yet) | one leg | D1 / D2 (other leg GND) |

- KY-040 R1 (SW pull-up) is unpopulated, so firmware uses `INPUT_PULLUP`. Power everything from 3V3.
- TM1637 is **not I2C** and is ~42 mm wide. It cannot be a slot module. It's breadboard-only.

### QT Py prototype pins (current firmware, direct-wired)
Encoder CLK/DT/SW = A0/A1/A2, Play = A3, Skip = TX, TM1637 CLK/DIO = SCK/MOSI. SDA/SCL left free.

## Firmware status

```
firmware/qtpy_media_remote/qtpy_media_remote.ino          # QT Py, BLE media remote, direct-wired KY-040 + keys + TM1637
firmware/esp8266_hardware_test/esp8266_hardware_test.ino  # ESP-12E, same logic, no BLE, Serial @115200
README.md                                                  # wiring, IDE setup, controls, troubleshooting
```

- Libraries: `ESP32 BLE Keyboard` (T-vK) and `TM1637` (Avishay Orpaz).
- Known caveat: ESP32 BLE Keyboard may fail on ESP32 Arduino core 3.x (`String` vs
  `std::string`). Use core **2.0.17** or a patched fork.
- Encoder: interrupt quadrature decode with table `{0,-1,1,0,1,0,0,-1,-1,0,0,1,0,1,-1,0}`,
  `ENC_STEPS_PER_DETENT=4`, `ENC_REVERSE=false`.
- `Button` struct: returns 0 = nothing, 1 = tap, 2 = hold (fires once while held). Debounce 25 ms.
- **Verification so far:** g++ compile against stub headers plus a logic simulation (all checks
  passed). The ESP-12E sketch also compiled in the real Arduino IDE (esp8266 core 3.1.2). **Never
  run on real hardware yet.**

### Joseph's Windows PC (Arduino IDE 2.3.10)
- Installed: esp8266 core 3.1.2, TM1637 1.2.0. Board selected: NodeMCU 1.0 (ESP-12E Module).
- The test sketch is saved as `Documents\Arduino\sketch_oct16besp8266_hardware_test`.
- **Blocker:** no COM port appears when the ESP-12E is plugged in. Likely a charge-only USB
  cable or a missing USB driver (CP2102 → Silicon Labs CP210x VCP; CH340 → WCH driver). Check
  Device Manager → Ports (COM & LPT). Joseph installs drivers himself.

## Display design chosen: Option A (car stereo) + settings menu (2026-10-30)
- Spec: `firmware/display/DISPLAY_SPEC.md`. Drawing code: `firmware/display/car_stereo_ui.h`.
  The drawing code is a 1:1 port of the canvas design and compiles cleanly against a stub GFX. It has never run on a real OLED.
- **Controls change:** holding the knob for **2 s opens the settings menu** (it used to be "sync"; sync is now menu item 1).
- Menu items:
  - Sync, brightness, volume step, knob reverse
  - Link mode: AUTO / BT / USB
  - BLUETOOTH: disconnect / forget phone
  - Module list, screensaver timeout, boot animation on/off, ghost segments on/off, about, exit
- Interactive design: https://claude.ai/artifact/5NmdTJPADnU77VvqSiS13y (artboards "Option A: …" and "All display states").

## Next tasks (priority order)

1. Get the ESP-12E port working, upload `esp8266_hardware_test`, and confirm the knob and display
   (tune `ENC_STEPS_PER_DETENT` / `ENC_REVERSE` if needed).
2. When the Adafruit parts arrive: **rewrite the QT Py firmware for the modular I2C
   architecture:**
   - PCA9548 channel scan at boot. Identify modules by address and probe: OLED 0x3C,
     seesaw encoder default 0x36, ATtiny817 seesaw default 0x49, mux 0x70.
   - Encoder via Adafruit seesaw (`getEncoderDelta`, button on seesaw pin 24). **Verify against
     the Adafruit guide.**
   - Keys via ATtiny817 seesaw: switch input + NeoKey NeoPixel. **Pin assignments not decided;
     check the Adafruit ATtiny817 and NeoKey guides.**
   - OLED via Adafruit SSD1306 on whichever mux channel it answers. Fallback to the onboard
     NeoPixel for status.
   - BLE HID media keys (keep the existing logic). Add **USB HID wired mode** (use USB when a
     host is detected, else BLE). Add **knob acceleration**.
   - **Verify** which I2C bus the QT Py ESP32-S3 STEMMA QT port uses (believed to be `Wire1`).
3. Test on a phone + BT speaker. On iPhone, check whether the on-screen keyboard hides when the
   BLE keyboard is connected.
4. Enclosure (still in design):
   - Draft: 224×136×36 mm, 40 mm slot grid, 13 slot positions, removable divider.
   - Possible combinable 1×2 slots (removable walls).
   - Filler blocks hold with friction fit or disc magnets, no connector.
   - Printers: Bambu P1S / A1 (256 mm bed), A1 mini (180 mm). Owned: M3 + M6 inserts, all M screws.
   - Print a single test slot first, to check connector fit and magnet hold.

## Cart (Adafruit, as of 2026-10-23, $93.60)

- 1× #5426 QT Py ESP32-S3
- 1× #5626 PCA9548
- 2× #4978 NeoKey socket
- 2× #5233 ATtiny817
- 1× #5880 I2C encoder
- 1× #6069 USB-C panel mount
- 1× #326 OLED
- 10× #4401 QT cable 200 mm
- 3× #5358 magnetic connector

**Recommended fixes (not yet applied):**
- Re-add #5039 keycaps ×1 and #5093 knob ×2 (they dropped out of the cart).
- Magnetic connectors 3 → 4 if the OLED is a module.
- Cables 10 → 6 is enough.

The cart lives in Joseph's Chrome, signed out. To share it, sign in → "Save as New Wishlist" → make it public.
UNG funding for parts is being explored before ordering.

**Non-Adafruit:** 26–28 AWG hookup wire, heat-shrink, CA glue/epoxy, solder, filament, a USB-C data cable.

## References

- Interactive parts guide + animated assembly (claude.ai artifact): https://claude.ai/artifact/ANRARMSssk3NyxkrdLkTgZ
  - Partly stale: still shows 2 muxes, 8 cables, the 1.3" OLED as optional, and pogo connectors as 6-pin.
- Earlier 3D model artifact (stale: backplane era): https://claude.ai/artifact/U3eASg8X3gTPWvZQGcBSzV

## Working preferences

- Explain step by step, plainly, without overwhelming detail. Be direct and honest about tradeoffs.
- Teammates are not tech-literate. Messages for them should avoid jargon.
- Weekly reports: formal register, no em dashes, don't include prices until asked, describe
  enclosure/firmware as in progress.
- Don't claim hardware behavior is verified unless it ran on a real board.
