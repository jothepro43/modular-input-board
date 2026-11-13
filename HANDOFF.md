# Handoff: where the panel firmware stands (2026-10-30)

Current firmware: `firmware/modular_remote/modular_remote.ino`, running on the board now.
Spec: `firmware/display/DISPLAY_SPEC.md`. The items below override the spec where they differ.

## Update 2026-11-13: Play and Skip keys added
- `modular_remote` now uses up to two key modules (0x49, switch on seesaw pin 18, LED on pin 19).
  Play/Pause and Skip (tap = next, hold 0.7 s = previous). In the menu, Play goes back.
- **Each key stores its own role** in byte 0 of its ATtiny's EEPROM (0xA0 Play, 0xA1 Skip), so the role
  follows the key to any port. A key with no role takes the free one (Play first) and keeps it.
  Menu > SWAP KEYS or serial `K` swaps them. Menu > MODULES shows each port as PLAY or SKIP.
  Key LED: dim green = Play, dim blue = Skip, white while pressed.
  The key on port 7 was labelled Play and the save read back correctly. Swap is untested.
- It also reads the phone's play state, so the screen shows `PAUSE` or `PLAY`, and logs the command
  IDs each app offers.
- Flashed and booted: mux, knob (port 0), OLED (port 3) and one key (port 7) found, iPhone reconnected.
  **The second key did not answer on any port.** No key press, LED colour or screen word has been
  checked on the board yet.
- No esptool 5 venv was on this Mac. `tools/upload.sh` needs one made each time or kept somewhere.

## What ran on the real board
- QT Py, PCA9548 and encoder: STEMMA QT port is `Wire1` (SDA1 41, SCL1 40). Mux 0x70, encoder 0x36.
- **OLED answers at 0x3D, not 0x3C.** Firmware accepts both.
- Encoder button is seesaw pin 24. Clockwise counts up with no reversal. One count per click.
  Product id reads 4991.
- Modules found by scanning every 2 s. Moving the knob and screen to new ports worked without a restart,
  with no stray commands on unplug.
- iPhone pairing, knob volume, real volume readback, iPhone buttons updating the screen,
  and automatic reconnect after a board restart.
- iPhone keyboard still appears in Notes with the panel connected.
- Song title and artist from Spotify, with scrolling. YouTube Music gave no title in one test, and the
  iPhone lock screen had no title either.
- Option A volume screen draws correctly. `car_stereo_ui.h` is unchanged and compiled as-is.

## Decisions made
- **Apple Media Service (AMS), not a BLE keyboard.** The keyboard approach failed to pair in testing
  (`Pairing FAILED, reason 0x55`, code not decoded), and a working one would likely hide the iPhone
  keyboard. AMS is iPhone only. The old approach is saved in `firmware/modular_remote_hid` and has
  never worked; its pairing mode still requires MITM.
- Normal screen is the song. The volume screen shows for 5 s after a change.
- Hold the knob **3 s** to open the menu (the spec says 2 s; the user asked for 3).
- Song info was added beyond the spec, because Option A has no room for it.
- Dropped: album cover (AMS sends text only; the OLED is 1-bit) and beat-following cow (the panel
  never hears the audio, and the user decided against a microphone).

## Where the menu differs from the spec
- SYNC VOL re-reads the phone's real volume instead of sending 25 volume-downs.
- STEP is 1 to 4 phone steps per click, because the iPhone moves in 1/16 steps.
- LINK defaults to BT, not AUTO. AUTO would control the Mac while plugged in for development.
- BLUETOOTH starts on CANCEL. DISCONNECT restarts the board into `BT OFF` (kept in RTC memory, cleared
  by a power cycle). In that state the first choice becomes TURN ON.
- No keys yet, so holding the knob closes the menu. Edits apply immediately and cannot be cancelled.
- Hint text still says `PLAY:BACK`, because `car_stereo_ui.h` was left unchanged.
- Mute has no AMS command, so it steps the volume to 0 and back up.

## Connecting phones
- The panel handles **one phone at a time** and stops advertising once one connects. A Mac scan could not
  see it while a phone was connecting.
- To switch iPhones, turn off Bluetooth on the connected one in Settings first, or it may reconnect first.
- The panel now also advertises HID (`0x1812`) and the generic HID appearance, as Espressif's AMS example
  does, so that an iPhone that has never paired can list it. It has no HID service.
- **Unconfirmed:** whether the on-screen keyboard still appears with this HID label. Check Notes. If it
  hides, undo this change. Also unconfirmed: that a second iPhone now lists and pairs with the panel.

## Next steps
1. **Check the on-screen keyboard in Notes** with the HID label (see above).
1. **Run the 7 checks the user has not reported on yet:**
   1. Volume shows, then music returns about 5 s after the last change. Is the scroll speed right now?
   2. 3 s hold opens the menu. 10 s idle closes it.
   3. BRIGHT and GHOST survive a power cycle.
   4. REVERSE flips the knob. STEP 2 moves two steps per click.
   5. Press shows `----` and `MUTE` and stays there. An earlier screenshot after `Mute` showed `0`,
      which may just be a knob turn clearing mute. Still unconfirmed.
   6. BLUETOOTH > DISCONNECT shows `BT OFF` and the iPhone stays off. TURN ON brings it back.
   7. BLUETOOTH > FORGET PHONE blinks `PAIR`. The iPhone must also Forget This Device before re-pairing.
2. **Step 4 of the display plan:** boot animation and screensaver. The SAVER and BOOT ANIM items only
   store a value so far. Boot should play only at power-on, which is why uploads will not show it.
3. Test LINK > USB. It is built but has never been tried. It should control the Mac's volume.
4. Log the exact commands each app offers before building Play and Skip. Counts seen: Music 2,
   Spotify 7, YouTube Music 9, Messages 9.
5. Not tested: NeoPixel status with no screen, a real speaker setup, other phones.

## Working on this machine (Mac)
- arduino-cli comes from Homebrew. Core is esp32 2.0.17. Libraries are Adafruit seesaw, SSD1306, GFX,
  NeoPixel. T-vK's BLE Keyboard is installed only for the HID sketch, and arduino-cli's
  `library.enable_unsafe_install` was turned on to install it.
- Board FQBN: `esp32:esp32:adafruit_qtpy_esp32s3_nopsram`.
- **After upload the board stays in its bootloader.** `tools/upload.sh` applies the fix: a watchdog reset
  from esptool 5 or newer, which needs a venv (`pip install pyserial esptool`).
- The serial port moved from `usbmodem101` to `usbmodem102` when USB HID was added. Do not hardcode it.
- macOS may ask for approval of USB accessories. Earlier serial trouble may have come from that prompt.
- Serial commands (115200):
  - `C` forget phones. `S` dump the screen. `R` re-read volume and track.
  - `m` open menu. `x` close it. `>` `<` `P` turn and press, menu only.
- Tools:
  - `tools/oled_screenshot.py out.png` saves what the firmware drew. It cannot detect a dead screen.
  - `tools/menu_walk.py out.png m ">" ">"` captures one frame per key string.
  - `tools/read_serial.py 30` reads serial for 30 s.
- **Do not send test commands while the user is using the board.** A test once collided with them and
  raised the iPhone's volume.
