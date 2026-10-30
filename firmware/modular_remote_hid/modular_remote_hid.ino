/*
  Modular Control Panel: modular I2C firmware
  ENGR 1101

  Board: Adafruit QT Py ESP32-S3 (No PSRAM). PCA9548 mux on the STEMMA QT port (Wire1).
  What it does:
    - Pairs with a phone or laptop over Bluetooth as a media remote (BLE HID).
    - Finds modules by scanning all 8 mux channels, at boot and every 2 s, so modules
      can sit on any port and can be moved while running.
    - I2C encoder (#5880): turn = volume up/down, press = mute, hold 1.5 s = sync volume to 0.
    - OLED (#326): volume estimate, OFF when muted, SYNC after a sync, pairing status.
      With no screen attached, the onboard NeoPixel shows pairing status instead
      (blinking blue = waiting to pair, green = connected).
    - Key modules (ATtiny817) are detected and logged only. Play/Skip come later.

  Libraries: ESP32 BLE Keyboard (T-vK), Adafruit seesaw, Adafruit SSD1306, Adafruit GFX,
  Adafruit NeoPixel. Use esp32 core 2.0.17.

  Important limit: Bluetooth media keys only send "volume up" and "volume down".
  The phone never reports its real volume back, so the number on screen is an estimate.
  Hold the knob 1.5 s to sync: the panel turns the phone all the way down and resets
  the estimate to 0.
*/

#include <Wire.h>
#include <BleKeyboard.h>
#include <BLEDevice.h>
#include <BLESecurity.h>
#include "Adafruit_seesaw.h"
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_NeoPixel.h>

// ---------------- I2C addresses ----------------
const uint8_t MUX_ADDR    = 0x70;
const uint8_t ENC_ADDR    = 0x36;
const uint8_t OLED_ADDR_A = 0x3C;
const uint8_t OLED_ADDR_B = 0x3D;  // the #326 on this board answers here
const uint8_t KEY_ADDR    = 0x49;
const uint8_t ENC_SWITCH  = 24;    // seesaw pin, per Adafruit's encoder_basic example

// ---------------- Tuning ----------------
const bool     ENC_REVERSE      = false; // true if clockwise turns the volume down
const uint8_t  VOL_STEP_PERCENT = 6;     // about one phone volume step (phones have ~16 steps)
const uint16_t DEBOUNCE_MS      = 25;
const uint16_t SYNC_HOLD_MS     = 1500;  // hold the knob this long to sync volume
const uint8_t  SYNC_PRESSES     = 25;    // volume-down presses sent during sync (more than any phone needs)
const uint16_t WORD_SHOW_MS     = 900;   // how long SYNC stays on screen
const uint16_t SCAN_EVERY_MS    = 2000;
const uint8_t  POLL_MS          = 10;
const uint8_t  KEY_HOLD_MS      = 20;    // gap after key down and after key up, so the phone sees both

BleKeyboard ble("Modular Panel", "ENGR 1101", 100);
Adafruit_seesaw encoder(&Wire1);
Adafruit_SSD1306 oled(128, 64, &Wire1, -1);
Adafruit_NeoPixel statusPixel(1, PIN_NEOPIXEL, NEO_GRB + NEO_KHZ800);

// ---------------- Bluetooth ----------------
void sendKey(const MediaKeyReport key) {
  ble.press(key);
  delay(KEY_HOLD_MS);
  ble.release(key);
  delay(KEY_HOLD_MS);
}

class PairingLog : public BLESecurityCallbacks {
  uint32_t onPassKeyRequest() override { return 0; }
  void onPassKeyNotify(uint32_t) override {}
  bool onSecurityRequest() override { return true; }
  bool onConfirmPIN(uint32_t) override { return true; }
  void onAuthenticationComplete(esp_ble_auth_cmpl_t result) override {
    if (result.success) Serial.printf("Pairing OK, auth mode 0x%02X\n", result.auth_mode);
    else Serial.printf("Pairing FAILED, reason 0x%02X\n", result.fail_reason);
  }
};

// ---------------- Debounced button with tap / hold ----------------
struct Button {
  uint16_t holdMs;
  bool     stable = false;      // true = pressed
  bool     lastRaw = false;
  uint32_t changedAt = 0;
  uint32_t pressedAt = 0;
  bool     holdFired = false;

  explicit Button(uint16_t hold) : holdMs(hold) {}

  // Returns: 0 nothing, 1 tap (released before hold time), 2 hold (fires once while still held)
  uint8_t update(bool raw, uint32_t now) {
    if (raw != lastRaw) { lastRaw = raw; changedAt = now; }
    if (now - changedAt < DEBOUNCE_MS) return 0;
    if (raw != stable) {
      stable = raw;
      if (stable) { pressedAt = now; holdFired = false; return 0; }
      return holdFired ? 0 : 1;                   // released: tap unless the hold already fired
    }
    if (stable && !holdFired && holdMs && now - pressedAt >= holdMs) { holdFired = true; return 2; }
    return 0;
  }

  void clear() { stable = lastRaw = holdFired = false; }
};

Button knobBtn(SYNC_HOLD_MS);

// ---------------- State ----------------
int8_t   encChannel = -1;       // -1 = not found
int8_t   oledChannel = -1;
uint8_t  keyChannels = 0;       // bitmask, logged only for now
bool     muxOk = false;
bool     wasConnected = false;
int      volume = 50;           // estimate, 0..100
bool     muted = false;
const char* shownWord = nullptr;
uint32_t wordUntil = 0;
uint32_t lastScan = 0;
uint32_t lastPixel = 0xFFFFFFFF;
bool     screenDirty = true;

// ---------------- Mux helpers ----------------
bool muxSelect(uint8_t ch) {
  Wire1.beginTransmission(MUX_ADDR);
  Wire1.write(1 << ch);
  return Wire1.endTransmission() == 0;
}

bool probe(uint8_t addr) {
  Wire1.beginTransmission(addr);
  return Wire1.endTransmission() == 0;
}

// ---------------- Module discovery ----------------
bool startEncoder(uint8_t ch) {
  if (!encoder.begin(ENC_ADDR)) return false;
  encoder.pinMode(ENC_SWITCH, INPUT_PULLUP);
  encoder.getEncoderDelta();    // clear anything counted before now
  knobBtn.clear();
  encChannel = ch;
  Serial.printf("Encoder found on channel %u, product id %u\n", ch,
                (unsigned)((encoder.getVersion() >> 16) & 0xFFFF));
  return true;
}

void loseEncoder() {
  Serial.printf("Encoder lost from channel %d\n", encChannel);
  encChannel = -1;
  knobBtn.clear();
  screenDirty = true;
}

bool startOled(uint8_t ch, uint8_t addr) {
  if (!oled.begin(SSD1306_SWITCHCAPVCC, addr, true, false)) return false;
  oledChannel = ch;
  screenDirty = true;
  Serial.printf("OLED found on channel %u at 0x%02X\n", ch, addr);
  return true;
}

void scanModules() {
  muxOk = muxSelect(0);
  if (!muxOk) {
    if (encChannel >= 0 || oledChannel >= 0) Serial.println("Mux lost");
    Serial.println("Mux NOT found at 0x70");
    encChannel = oledChannel = -1;
    keyChannels = 0;
    return;
  }

  if (oledChannel >= 0) {
    muxSelect(oledChannel);
    if (!probe(OLED_ADDR_A) && !probe(OLED_ADDR_B)) {
      Serial.printf("OLED lost from channel %d\n", oledChannel);
      oledChannel = -1;
    }
  }

  uint8_t keysNow = 0;
  for (uint8_t ch = 0; ch < 8; ch++) {
    muxSelect(ch);
    if (encChannel < 0 && probe(ENC_ADDR)) startEncoder(ch);
    if (oledChannel < 0) {
      if (probe(OLED_ADDR_B)) startOled(ch, OLED_ADDR_B);
      else if (probe(OLED_ADDR_A)) startOled(ch, OLED_ADDR_A);
    }
    if (probe(KEY_ADDR)) keysNow |= 1 << ch;
  }
  if (keysNow != keyChannels) {
    keyChannels = keysNow;
    Serial.printf("Key modules on channel mask 0x%02X (not used yet)\n", keyChannels);
  }
}

// ---------------- Screen / status ----------------
void showWord(const char* word, uint32_t now) {
  shownWord = word;
  wordUntil = now + WORD_SHOW_MS;
  screenDirty = true;
}

void drawScreen(bool connected) {
  if (oledChannel < 0 || !muxSelect(oledChannel)) return;

  oled.clearDisplay();
  oled.setTextColor(SSD1306_WHITE);
  oled.setTextSize(1);
  oled.setCursor(0, 0);
  oled.print(connected ? "CONNECTED" : "BLUETOOTH");
  if (encChannel < 0) {
    oled.setCursor(84, 0);
    oled.print("NO KNOB");
  }

  if (shownWord) {
    oled.setTextSize(4);
    oled.setCursor(0, 24);
    oled.print(shownWord);
  } else if (!connected) {
    oled.setTextSize(2);
    oled.setCursor(0, 20);
    oled.print("PAIRING");
    oled.setTextSize(1);
    oled.setCursor(0, 46);
    oled.print("Modular Panel");
  } else if (muted) {
    oled.setTextSize(4);
    oled.setCursor(0, 24);
    oled.print("OFF");
  } else {
    oled.setTextSize(4);
    oled.setCursor(0, 16);
    oled.print(volume);
    oled.drawRect(0, 56, 128, 8, SSD1306_WHITE);
    oled.fillRect(0, 56, 128 * volume / 100, 8, SSD1306_WHITE);
  }
  oled.display();
}

void updatePixel(bool connected, uint32_t now) {
  uint32_t want = 0;  // off while a screen is showing status
  if (oledChannel < 0) {
    if (connected) want = statusPixel.Color(0, 255, 0);
    else if ((now / 500) % 2) want = statusPixel.Color(0, 0, 255);
  }
  if (want == lastPixel) return;
  lastPixel = want;
  statusPixel.setPixelColor(0, want);
  statusPixel.show();
}

// ---------------- Knob ----------------
void handleTurn(int32_t delta, bool connected) {
  if (delta == 0) return;
  if (!connected) {
    Serial.println("Turn ignored: not connected");
    return;
  }
  if (ENC_REVERSE) delta = -delta;
  while (delta != 0) {
    if (delta > 0) { sendKey(KEY_MEDIA_VOLUME_UP);   volume = min(100, volume + VOL_STEP_PERCENT); delta--; }
    else           { sendKey(KEY_MEDIA_VOLUME_DOWN); volume = max(0,   volume - VOL_STEP_PERCENT); delta++; }
  }
  muted = false;                // phones unmute when the volume changes
  shownWord = nullptr;          // end any word on screen so the number comes back
  screenDirty = true;
  Serial.printf("Volume estimate %d\n", volume);
}

void handleButton(uint8_t action, bool connected, uint32_t now) {
  if (action == 0) return;
  if (!connected) {
    Serial.println(action == 1 ? "Press ignored: not connected" : "Hold ignored: not connected");
    return;
  }
  if (action == 1) {
    sendKey(KEY_MEDIA_MUTE);
    muted = !muted;
    screenDirty = true;
    Serial.println(muted ? "Mute" : "Unmute");
  } else {
    showWord("SYNC", now);
    drawScreen(connected);
    screenDirty = false;
    for (uint8_t i = 0; i < SYNC_PRESSES; i++) sendKey(KEY_MEDIA_VOLUME_DOWN);
    volume = 0;
    muted = false;
    screenDirty = true;
    Serial.println("Volume synced to 0");
  }
}

void pollEncoder(bool connected, uint32_t now) {
  if (encChannel < 0) return;
  muxSelect(encChannel);
  if (!probe(ENC_ADDR)) { loseEncoder(); return; }

  int32_t delta = encoder.getEncoderDelta();
  bool pressed = !encoder.digitalRead(ENC_SWITCH);

  // A module pulled out mid-read returns junk, which could look like a press or a big turn.
  if (!probe(ENC_ADDR)) { loseEncoder(); return; }

  handleTurn(delta, connected);
  handleButton(knobBtn.update(pressed, now), connected, now);
}

void setup() {
  Serial.begin(115200);
  unsigned long t0 = millis();
  while (!Serial && millis() - t0 < 3000) delay(10);

  pinMode(NEOPIXEL_POWER, OUTPUT);
  digitalWrite(NEOPIXEL_POWER, NEOPIXEL_POWER_ON);
  statusPixel.begin();
  statusPixel.setBrightness(20);

  Wire1.begin(SDA1, SCL1);
  scanModules();

  BLEDevice::setSecurityCallbacks(new PairingLog());
  ble.begin();
  Serial.println("Modular Panel ready. Pair with \"Modular Panel\" in your phone's Bluetooth settings.");
}

void loop() {
  uint32_t now = millis();
  bool connected = ble.isConnected();

  if (connected != wasConnected) {
    wasConnected = connected;
    screenDirty = true;
    Serial.println(connected ? "Bluetooth connected" : "Bluetooth disconnected");
  }

  if (now - lastScan >= SCAN_EVERY_MS) {
    lastScan = now;
    scanModules();
    Serial.printf("[alive] mux=%s encoder=%d oled=%d keys=0x%02X bt=%s vol=%d%s\n", muxOk ? "ok" : "MISSING",
                  encChannel, oledChannel, keyChannels, connected ? "connected" : "waiting", volume, muted ? " muted" : "");
  }

  pollEncoder(connected, now);

  if (shownWord && now >= wordUntil) { shownWord = nullptr; screenDirty = true; }
  if (screenDirty) { screenDirty = false; drawScreen(connected); }
  updatePixel(connected, now);

  delay(POLL_MS);
}
