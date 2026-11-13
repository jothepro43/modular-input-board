// Bring-up test: one key module (NeoKey socket #4978 + ATtiny817 seesaw #5233),
// plugged straight into the QT Py ESP32-S3 STEMMA QT port (no mux).
// If the PCA9548 mux is connected, the sketch also checks its 8 ports for the key.
// Serial 115200: I2C scan, seesaw product id, PRESSED / RELEASED on change.
// LED: red, green, blue at boot, then white while pressed and dim blue while released.
//
// Wiring: NeoKey + -> ATtiny 3v, - -> G, I -> seesaw pin 19, A -> seesaw pin 18, C -> - (jumper).
// If presses never register, set USE_PULLDOWN to 1 and rewire: A -> 3v, C -> pin 18.
#include <Wire.h>
#include "Adafruit_seesaw.h"
#include "seesaw_neopixel.h"

#define USE_PULLDOWN 0

const uint8_t KEY_ADDR   = 0x49;
const uint8_t SWITCH_PIN = 18;
const uint8_t PIXEL_PIN  = 19;
const unsigned long DEBOUNCE_MS = 20;

// One object does both jobs: seesaw_NeoPixel is an Adafruit_seesaw, so it also has
// pinMode / digitalRead. Using two objects would reset the ATtiny twice, because
// begin() sends a software reset, and the second reset would clear the pin setup.
seesaw_NeoPixel key(1, PIXEL_PIN, NEO_GRB + NEO_KHZ800, &Wire1);

seesaw_NeoPixel encPixel(1, 6, NEO_GRB + NEO_KHZ800, &Wire1);  // knob board, for the "E" control test

bool keyOk = false;
bool stablePressed = false;
bool lastRaw = false;
unsigned long rawChangedAt = 0;
unsigned long lastScan = 0;
unsigned long lastHeartbeat = 0;
uint32_t pressCount = 0;

void scanBus() {
  Serial.print("I2C scan (Wire1):");
  int found = 0;
  for (uint8_t a = 1; a < 127; a++) {
    Wire1.beginTransmission(a);
    if (Wire1.endTransmission() == 0) {
      Serial.printf(" 0x%02X", a);
      found++;
    }
  }
  Serial.println(found ? "" : " nothing found");
}

// If the PCA9548 mux (0x70) is on the bus, look for the key behind each of its 8 ports.
// Returns the port the key answers on and leaves it selected, -1 if direct or not found.
const uint8_t MUX_ADDR = 0x70;
int keyChannel = -1;

bool present(uint8_t addr) {
  Wire1.beginTransmission(addr);
  return Wire1.endTransmission() == 0;
}

void muxSelect(uint8_t mask) {
  Wire1.beginTransmission(MUX_ADDR);
  Wire1.write(mask);
  Wire1.endTransmission();
}

void findKeyBehindMux() {
  keyChannel = -1;
  if (!present(MUX_ADDR)) return;
  muxSelect(0);  // close every port so the scan below only sees one at a time
  if (present(KEY_ADDR)) { Serial.println("0x49 answers with all mux ports closed (direct)"); return; }
  for (uint8_t ch = 0; ch < 8; ch++) {
    muxSelect(1 << ch);
    Serial.printf("  mux port %u:", ch);
    for (uint8_t a = 1; a < 127; a++)
      if (a != MUX_ADDR && present(a)) Serial.printf(" 0x%02X", a);
    Serial.println();
    if (keyChannel < 0 && present(KEY_ADDR)) keyChannel = ch;
  }
  if (keyChannel >= 0) {
    muxSelect(1 << keyChannel);
    Serial.printf("Key found behind mux port %d\n", keyChannel);
  } else {
    muxSelect(0);
  }
}

void setColor(uint8_t r, uint8_t g, uint8_t b) {
  key.setPixelColor(0, key.Color(r, g, b));
  key.show();
}

bool readPressed() {
  bool level = key.digitalRead(SWITCH_PIN);
#if USE_PULLDOWN
  return level;   // pressed pulls the pin up to 3v
#else
  return !level;  // pressed pulls the pin down through the diode
#endif
}

bool startKey() {
  findKeyBehindMux();
  if (!key.begin(KEY_ADDR)) return false;
  uint32_t version = key.getVersion();
  Serial.printf("seesaw found at 0x%02X, product id %u, date code %u\n", KEY_ADDR,
                (unsigned)((version >> 16) & 0xFFFF), (unsigned)(version & 0xFFFF));
#if USE_PULLDOWN
  key.pinMode(SWITCH_PIN, INPUT_PULLDOWN);
  Serial.println("Switch: pin 18 INPUT_PULLDOWN, pressed = HIGH");
#else
  key.pinMode(SWITCH_PIN, INPUT_PULLUP);
  Serial.println("Switch: pin 18 INPUT_PULLUP, pressed = LOW");
#endif
  key.setBrightness(255);
  Serial.println("Boot colors: red, green, blue");
  setColor(255, 0, 0); delay(300);
  setColor(0, 255, 0); delay(300);
  setColor(0, 0, 255); delay(300);
  lastRaw = stablePressed = readPressed();
  setColor(stablePressed ? 255 : 0, stablePressed ? 255 : 0, stablePressed ? 255 : 20);
  Serial.printf("Ready. Key starts %s\n", stablePressed ? "PRESSED" : "RELEASED");
  return true;
}

void setup() {
  Serial.begin(115200);
  unsigned long t0 = millis();
  while (!Serial && millis() - t0 < 3000) delay(10);
  Serial.println("\nkey_module_test");
  Wire1.begin(SDA1, SCL1);  // STEMMA QT port on the QT Py ESP32-S3: SDA1 = 41, SCL1 = 40
  scanBus();
  keyOk = startKey();
  if (!keyOk) Serial.println("seesaw NOT found at 0x49. Rescanning every 2 s, reseat the cable.");
}

void loop() {
  unsigned long now = millis();

  if (!keyOk) {
    if (now - lastScan >= 2000) {
      lastScan = now;
      scanBus();
      keyOk = startKey();
    }
    delay(10);
    return;
  }

  if (now - lastHeartbeat >= 1000) {
    lastHeartbeat = now;
    Serial.printf("[alive] presses=%lu  pin18 raw=%d\n", (unsigned long)pressCount,
                  (int)key.digitalRead(SWITCH_PIN));
  }

  int cmd = Serial.available() ? Serial.read() : -1;

  // "E" plays the same colors on the knob board's NeoPixel (seesaw pin 6, per Adafruit's
  // encoder_basic example). Control test: if the knob lights and the key doesn't, the code is fine.
  if (cmd == 'E') {
    int encCh = -1;
    for (uint8_t ch = 0; ch < 8 && encCh < 0; ch++) {
      muxSelect(1 << ch);
      if (present(0x36)) encCh = ch;
    }
    if (encCh < 0) {
      Serial.println("Knob (0x36) not found");
    } else if (!encPixel.begin(0x36)) {
      Serial.println("Knob seesaw did not start");
    } else {
      Serial.printf("Knob on mux port %d: red, green, blue, off\n", encCh);
      encPixel.setBrightness(255);
      uint32_t colors[] = {encPixel.Color(255, 0, 0), encPixel.Color(0, 255, 0), encPixel.Color(0, 0, 255), 0};
      for (uint32_t c : colors) { encPixel.setPixelColor(0, c); encPixel.show(); delay(1000); }
    }
    if (keyChannel >= 0) muxSelect(1 << keyChannel);
  }

  // "L" replays the boot colors so you can watch the LED
  if (cmd == 'L') {
    Serial.println("Colors: red, green, blue");
    setColor(255, 0, 0); delay(1000);
    setColor(0, 255, 0); delay(1000);
    setColor(0, 0, 255); delay(1000);
    setColor(0, 0, 20);
  }

  bool raw = readPressed();
  if (raw != lastRaw) {
    lastRaw = raw;
    rawChangedAt = now;
  }
  if (raw != stablePressed && now - rawChangedAt >= DEBOUNCE_MS) {
    stablePressed = raw;
    if (stablePressed) pressCount++;
    Serial.printf("%s  (press #%lu)\n", stablePressed ? "PRESSED" : "RELEASED", (unsigned long)pressCount);
    if (stablePressed) setColor(255, 255, 255);
    else setColor(0, 0, 20);
  }
  delay(2);
}
