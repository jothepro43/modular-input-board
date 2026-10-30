# Display spec: Option A (car stereo) + settings menu

Target: Adafruit 0.96" 128×64 SSD1306 OLED (#326), white, I2C 0x3C, on a PCA9548 mux channel.
Design reference (interactive): https://claude.ai/artifact/5NmdTJPADnU77VvqSiS13y
Artboards: "Option A: Car-stereo boot animation", "All display states", "Option A: Menu".
Drawing code: `car_stereo_ui.h` in this folder. It is a 1:1 port of the design and compile-checked (C++11, -Wall -Wextra) against a stub GFX.
**It has not run on a real OLED yet.**

## Rendering rules
- Libraries: Adafruit_SSD1306 + Adafruit_GFX. Each frame is `clearDisplay()`, then one `carui::draw*()`, then `display()`.
- Select the OLED's mux channel before every `display()`.
- Frame rate: about 20 fps during animations. Redraw only when state changes otherwise.
- `carui::ghost()` controls the faint unlit segments. The GHOST menu item toggles it.
- Unlit wedge bars are drawn as outlines; that is on purpose.

## Screens
| State | Call |
|---|---|
| Boot (once at power-on, if BOOT ANIM = ON) | `drawBoot(d, millis() - bootStart, vol)` until it returns false (about 10.9 s). Any knob or key input skips it. |
| Volume | `drawVolume(d, vol, btConnected)` |
| Muted | `drawMuted(d, btConnected)` |
| Play/Pause pressed | `drawWord(d, "PLAY", vol, bt)` for 900 ms |
| Skip tap / hold | `drawWord(d, "SEEK>" / "<SEEK", vol, bt)` for 900 ms |
| Sync | `drawWord(d, "SYNC", vol, bt)` for 900 ms |
| Not connected / pairing | `drawPairing(d, millis())` (blinks PAIR and BT) |
| Screensaver | `drawStandby(d)` at low contrast, or display off. Wake on any input; that input is not acted on. |
| Menu | `drawMenu(d, index, count, label, value, bar, editing, millis())` |

## Controls
**Normal mode**
- **Knob:**
  - Turn: volume ± STEP (media keys).
  - Press: mute.
  - **Hold 2 s: open the settings menu.** This replaces "hold = sync"; sync is now menu item 1.
- **Play key:** play/pause.
- **Skip key:** tap = next, hold 0.7 s = previous.

**Menu mode** (no media keys are sent while in the menu)
- **Knob turn:** move to the previous/next item. While editing, it changes the value instead.
- **Knob press:**
  - Range or option item: start or stop editing.
  - Action item: run it.
- **Play key:** back. While editing, it cancels the edit; otherwise it leaves the menu.
- **Skip key:** ignored.
- **Auto-exit:** 10 s with no input closes the menu and saves.

## Menu items (in order)
| # | Label | Kind | Values | What it does |
|---|---|---|---|---|
| 1 | SYNC VOL | action | | Send 25× volume-down, set vol = 0, flash SYNC |
| 2 | BRIGHT | range 1–20, shows wedge | default 14 | `ssd1306_command(SSD1306_SETCONTRAST)` then `ssd1306_command(bright*12+15)` |
| 3 | STEP | range 1–10 (%) | default 5 | Volume change per knob click |
| 4 | REVERSE | option | OFF / ON | Flip knob direction |
| 5 | LINK | option | AUTO / BT / USB | AUTO = USB HID if a USB host is present, else BLE |
| 6 | BLUETOOTH | choose | DISCONNECT / FORGET PHONE / CANCEL | Shows CONNECTED or NOT CONNECTED when not editing. **DISCONNECT:** drop the BLE connection and stop advertising until the user picks BLUETOOTH again or power-cycles. **FORGET PHONE:** delete BLE bonds, restart advertising, show the pairing screen. |
| 7 | MODULES | list (view only) | "S1 KNOB", "S2 OLED", … | Built from the mux channel scan: encoder 0x36 = KNOB, ATtiny 0x49 = KEY, 0x3C = OLED, nothing = EMPTY |
| 8 | SAVER | option | OFF / 1 MIN / 5 MIN / 10 MIN | Screensaver timeout (protects the OLED from burn-in) |
| 9 | BOOT ANIM | option | ON / OFF | Play the boot animation at power-on |
| 10 | GHOST | option | ON / OFF | `carui::ghost()` |
| 11 | ABOUT | info | "TAAC FW 0.1" | Firmware version |
| 12 | EXIT | action | | Leave the menu |

- Persist settings in NVS with `Preferences.h` (namespace "taac") when the menu closes.
- Labels are at most 9 characters (size 2 text after the ">" marker).
- Values are at most 15 characters.

## Implementation notes
- Use a small state machine: `BOOT → NORMAL ⇄ MENU (browse ⇄ edit)`, plus a transient `WORD` overlay and `SAVER`.
- **Long press:** detect the 2 s knob hold with the existing `Button` struct (`holdMs = 2000`). The tap-vs-hold logic already fires hold once and suppresses the tap.
- **Bluetooth disconnect:** check which BLE stack is in use. ESP32 BLE Keyboard (T-vK) wraps NimBLE or Bluedroid; the disconnect and bond-delete calls differ by stack.
- **USB mode:** needs the ESP32-S3 TinyUSB HID (USB "Consumer Control" for media keys).
- **Test order:**
  1. Volume screen
  2. Menu browse
  3. Each setting
  4. Bluetooth disconnect / forget
  5. Boot
  6. Saver
