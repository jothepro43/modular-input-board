/*
  Modular Control Panel — prototype firmware
  ENGR 1101

  Board:   Adafruit QT Py ESP32-S3 (No PSRAM)
  What it does:
    - Pairs with a phone or laptop over Bluetooth as a media remote (BLE HID).
    - KY-040 encoder: turn = volume up/down, press = mute, hold 1.5 s = sync volume to 0.
    - Key 1 (Play/Pause): play or pause.
    - Key 2 (Skip): tap = next track, hold 0.7 s = previous track.
    - TM1637 4-digit display: shows the volume estimate, OFF when muted,
      and short words (PLAY, SKIP, PrEV, SYnC) after each action.

  Libraries (Arduino IDE > Library Manager):
    - "ESP32 BLE Keyboard" by T-vK            (BleKeyboard.h)
    - "TM1637" by Avishay Orpaz                (TM1637Display.h)

  Important limit: Bluetooth media keys only send "volume up" and "volume down".
  The phone never reports its real volume back, so the number on the display is
  an estimate. Hold the knob 1.5 s to sync: the panel turns the phone all the way
  down and resets the estimate to 0.
*/

#include <BleKeyboard.h>
#include <TM1637Display.h>

// ---------------- Pins (QT Py ESP32-S3 silkscreen names) ----------------
// SDA and SCL are left free for the I2C modules later.
const uint8_t PIN_ENC_CLK  = A0;   // KY-040 CLK
const uint8_t PIN_ENC_DT   = A1;   // KY-040 DT
const uint8_t PIN_ENC_SW   = A2;   // KY-040 SW (R1 is empty on this module, so we use INPUT_PULLUP)
const uint8_t PIN_KEY_PLAY = A3;   // Keychron switch: one leg here, other leg to GND
const uint8_t PIN_KEY_SKIP = TX;   // Keychron switch: one leg here, other leg to GND
const uint8_t PIN_TM_CLK   = SCK;  // TM1637 CLK
const uint8_t PIN_TM_DIO   = MOSI; // TM1637 DIO
// Power: KY-040 "+" and TM1637 VCC go to the QT Py 3V pin, not 5V.

// ---------------- Tuning ----------------
const int8_t   ENC_STEPS_PER_DETENT = 4;     // KY-040 is usually 4. If one click counts as 2, set 2. If it needs two clicks, set 2.
const bool     ENC_REVERSE          = false; // true if clockwise turns the volume down
const uint8_t  VOL_STEP_PERCENT     = 6;     // about one phone volume step (phones have ~16 steps)
const uint16_t DEBOUNCE_MS          = 25;
const uint16_t SKIP_HOLD_MS         = 700;   // hold Skip this long for previous track
const uint16_t SYNC_HOLD_MS         = 1500;  // hold the knob this long to sync volume
const uint8_t  SYNC_PRESSES         = 25;    // volume-down presses sent during sync (more than any phone needs)
const uint16_t WORD_SHOW_MS         = 900;   // how long PLAY / SKIP / etc. stay on screen
const uint8_t  DISPLAY_BRIGHTNESS   = 5;     // 0..7

BleKeyboard ble("Modular Panel", "ENGR 1101", 100);
TM1637Display display(PIN_TM_CLK, PIN_TM_DIO);

// ---------------- Display words (segments: gfedcba) ----------------
const uint8_t W_PLAY[] = {0x73, 0x38, 0x77, 0x6E};  // PLAY
const uint8_t W_SKIP[] = {0x6D, 0x75, 0x30, 0x73};  // SKIP (K and I are 7-segment approximations)
const uint8_t W_PREV[] = {0x73, 0x50, 0x79, 0x3E};  // PrEV
const uint8_t W_SYNC[] = {0x6D, 0x6E, 0x54, 0x39};  // SYnC
const uint8_t W_OFF[]  = {0x00, 0x3F, 0x71, 0x71};  //  OFF
const uint8_t W_BT[]   = {0x40, 0x7C, 0x78, 0x40};  // -bt-
const uint8_t W_BLANK[] = {0x00, 0x00, 0x00, 0x00};

// ---------------- Encoder (interrupt driven, full quadrature decode) ----------------
volatile int8_t  encAccum  = 0;   // transitions since the last full detent
volatile int16_t encDetents = 0;  // detents not yet handled by loop()
volatile uint8_t encPrev   = 0;
portMUX_TYPE encMux = portMUX_INITIALIZER_UNLOCKED;

// Valid quadrature transitions give +1 or -1; bounces and skipped states give 0.
const int8_t QUAD_TABLE[16] = {0, -1, 1, 0, 1, 0, 0, -1, -1, 0, 0, 1, 0, 1, -1, 0};

void IRAM_ATTR onEncoderChange() {
  uint8_t now = (digitalRead(PIN_ENC_CLK) << 1) | digitalRead(PIN_ENC_DT);
  int8_t step = QUAD_TABLE[(encPrev << 2) | now];
  encPrev = now;
  if (step == 0) return;
  portENTER_CRITICAL_ISR(&encMux);
  encAccum += step;
  if (encAccum >= ENC_STEPS_PER_DETENT)       { encDetents++; encAccum = 0; }
  else if (encAccum <= -ENC_STEPS_PER_DETENT) { encDetents--; encAccum = 0; }
  portEXIT_CRITICAL_ISR(&encMux);
}

int16_t takeDetents() {
  portENTER_CRITICAL(&encMux);
  int16_t d = encDetents;
  encDetents = 0;
  portEXIT_CRITICAL(&encMux);
  return ENC_REVERSE ? -d : d;
}

// ---------------- Debounced button with tap / hold ----------------
struct Button {
  uint8_t  pin;
  uint16_t holdMs;
  bool     stable = false;      // true = pressed
  bool     lastRaw = false;
  uint32_t changedAt = 0;
  uint32_t pressedAt = 0;
  bool     holdFired = false;

  // Returns: 0 nothing, 1 tap (released before hold time), 2 hold (fires once while still held)
  uint8_t update(uint32_t now) {
    bool raw = digitalRead(pin) == LOW;           // pressed pulls the pin to GND
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
};

Button knobBtn{PIN_ENC_SW,   SYNC_HOLD_MS};
Button playBtn{PIN_KEY_PLAY, 0};
Button skipBtn{PIN_KEY_SKIP, SKIP_HOLD_MS};

// ---------------- State ----------------
int      volume = 50;           // estimate, 0..100
bool     muted = false;
uint32_t wordUntil = 0;         // while now < wordUntil, a word is on screen
const uint8_t* shownWord = nullptr;

void showWord(const uint8_t* w, uint32_t now) {
  shownWord = w;
  wordUntil = now + WORD_SHOW_MS;
  display.setSegments(w);
}

void sendKey(const MediaKeyReport key) {
  if (ble.isConnected()) ble.write(key);
}

void refreshDisplay(uint32_t now) {
  static int lastShown = -2;     // -1 = OFF, -2 = nothing yet, -3 = blinking bt, 0..100 = number
  if (now < wordUntil) return;   // a word is still on screen
  bool dirty = false;
  if (shownWord) { shownWord = nullptr; dirty = true; }   // a word just expired: redraw
  if (!ble.isConnected()) {
    // Blink -bt- while waiting to pair
    bool on = (now / 500) % 2;
    display.setSegments(on ? W_BT : W_BLANK);
    lastShown = -3;
    return;
  }
  int want = muted ? -1 : volume;
  if (!dirty && want == lastShown) return;
  lastShown = want;
  if (want == -1) display.setSegments(W_OFF);
  else display.showNumberDec(want, false);
}

void setup() {
  Serial.begin(115200);

  pinMode(PIN_ENC_CLK, INPUT_PULLUP);  // the module has pull-ups on CLK/DT; these are a harmless backup
  pinMode(PIN_ENC_DT, INPUT_PULLUP);
  pinMode(PIN_ENC_SW, INPUT_PULLUP);   // needed: R1 (the SW pull-up) is not fitted on this KY-040
  pinMode(PIN_KEY_PLAY, INPUT_PULLUP);
  pinMode(PIN_KEY_SKIP, INPUT_PULLUP);

  encPrev = (digitalRead(PIN_ENC_CLK) << 1) | digitalRead(PIN_ENC_DT);
  attachInterrupt(digitalPinToInterrupt(PIN_ENC_CLK), onEncoderChange, CHANGE);
  attachInterrupt(digitalPinToInterrupt(PIN_ENC_DT), onEncoderChange, CHANGE);

  display.setBrightness(DISPLAY_BRIGHTNESS, true);
  display.setSegments(W_BT);

  ble.begin();
  Serial.println("Modular Panel ready. Pair with \"Modular Panel\" in your phone's Bluetooth settings.");
}

void loop() {
  uint32_t now = millis();

  // Volume knob
  int16_t d = takeDetents();
  while (d != 0) {
    if (d > 0) { sendKey(KEY_MEDIA_VOLUME_UP);   volume = min(100, volume + VOL_STEP_PERCENT); d--; }
    else       { sendKey(KEY_MEDIA_VOLUME_DOWN); volume = max(0,   volume - VOL_STEP_PERCENT); d++; }
    muted = false;               // phones unmute when the volume changes
    wordUntil = 0;               // end any word on screen so the number comes back
    delay(8);                   // small gap so fast spins are not dropped by the phone
  }

  // Knob press: tap = mute, hold = sync volume to 0
  switch (knobBtn.update(now)) {
    case 1:
      sendKey(KEY_MEDIA_MUTE);
      muted = !muted;
      Serial.println(muted ? "Mute" : "Unmute");
      break;
    case 2:
      showWord(W_SYNC, now);
      for (uint8_t i = 0; i < SYNC_PRESSES; i++) { sendKey(KEY_MEDIA_VOLUME_DOWN); delay(30); }
      volume = 0; muted = false;
      Serial.println("Volume synced to 0");
      break;
  }

  // Play / Pause
  if (playBtn.update(now) == 1) {
    sendKey(KEY_MEDIA_PLAY_PAUSE);
    showWord(W_PLAY, now);
    Serial.println("Play/Pause");
  }

  // Skip: tap = next, hold = previous
  switch (skipBtn.update(now)) {
    case 1: sendKey(KEY_MEDIA_NEXT_TRACK);     showWord(W_SKIP, now); Serial.println("Next");     break;
    case 2: sendKey(KEY_MEDIA_PREVIOUS_TRACK); showWord(W_PREV, now); Serial.println("Previous"); break;
  }

  refreshDisplay(now);
  delay(2);
}
