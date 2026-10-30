/*
  Modular Control Panel — hardware test for the ESP-12E DevKit V2 (ESP8266)
  ENGR 1101

  Use this while the QT Py ships. The ESP8266 has no Bluetooth, so it cannot
  control a speaker, but it proves the wiring and the input/display logic:
    - Turn the knob: the display counts 0..100 and Serial prints "Volume up/down".
    - Press the knob: display shows OFF (mute) / the number again.
    - Hold the knob 1.5 s: SYnC, volume resets to 0.
    - Play key: PLAY.   Skip key: tap = SKIP, hold 0.7 s = PrEV.

  Board:   "NodeMCU 1.0 (ESP-12E Module)" from the esp8266 board package
  Library: "TM1637" by Avishay Orpaz (TM1637Display.h)
  Serial Monitor: 115200 baud
*/

#include <TM1637Display.h>

// ---------------- Pins (silkscreen names on the DevKit) ----------------
// D3 and D4 must be HIGH at boot. The TM1637 lines idle high, so they are safe there.
const uint8_t PIN_ENC_CLK  = D5;   // KY-040 CLK
const uint8_t PIN_ENC_DT   = D6;   // KY-040 DT
const uint8_t PIN_ENC_SW   = D7;   // KY-040 SW (INPUT_PULLUP, R1 is empty on the module)
const uint8_t PIN_KEY_PLAY = D1;   // switch to GND
const uint8_t PIN_KEY_SKIP = D2;   // switch to GND
const uint8_t PIN_TM_CLK   = D3;   // TM1637 CLK
const uint8_t PIN_TM_DIO   = D4;   // TM1637 DIO
// Power: KY-040 "+" and TM1637 VCC to 3V3, GND to GND.

const int8_t   ENC_STEPS_PER_DETENT = 4;
const bool     ENC_REVERSE          = false;
const uint8_t  VOL_STEP_PERCENT     = 6;
const uint16_t DEBOUNCE_MS          = 25;
const uint16_t SKIP_HOLD_MS         = 700;
const uint16_t SYNC_HOLD_MS         = 1500;
const uint16_t WORD_SHOW_MS         = 900;

TM1637Display display(PIN_TM_CLK, PIN_TM_DIO);

const uint8_t W_PLAY[] = {0x73, 0x38, 0x77, 0x6E};
const uint8_t W_SKIP[] = {0x6D, 0x75, 0x30, 0x73};
const uint8_t W_PREV[] = {0x73, 0x50, 0x79, 0x3E};
const uint8_t W_SYNC[] = {0x6D, 0x6E, 0x54, 0x39};
const uint8_t W_OFF[]  = {0x00, 0x3F, 0x71, 0x71};
const uint8_t W_TEST[] = {0x78, 0x79, 0x6D, 0x78};  // tESt

volatile int8_t  encAccum = 0;
volatile int16_t encDetents = 0;
volatile uint8_t encPrev = 0;
const int8_t QUAD_TABLE[16] = {0, -1, 1, 0, 1, 0, 0, -1, -1, 0, 0, 1, 0, 1, -1, 0};

void IRAM_ATTR onEncoderChange() {
  uint8_t now = (digitalRead(PIN_ENC_CLK) << 1) | digitalRead(PIN_ENC_DT);
  int8_t step = QUAD_TABLE[(encPrev << 2) | now];
  encPrev = now;
  if (step == 0) return;
  encAccum += step;
  if (encAccum >= ENC_STEPS_PER_DETENT)       { encDetents++; encAccum = 0; }
  else if (encAccum <= -ENC_STEPS_PER_DETENT) { encDetents--; encAccum = 0; }
}

int16_t takeDetents() {
  noInterrupts();
  int16_t d = encDetents;
  encDetents = 0;
  interrupts();
  return ENC_REVERSE ? -d : d;
}

struct Button {
  uint8_t  pin;
  uint16_t holdMs;
  bool     stable = false;
  bool     lastRaw = false;
  uint32_t changedAt = 0;
  uint32_t pressedAt = 0;
  bool     holdFired = false;

  uint8_t update(uint32_t now) {
    bool raw = digitalRead(pin) == LOW;
    if (raw != lastRaw) { lastRaw = raw; changedAt = now; }
    if (now - changedAt < DEBOUNCE_MS) return 0;
    if (raw != stable) {
      stable = raw;
      if (stable) { pressedAt = now; holdFired = false; return 0; }
      return holdFired ? 0 : 1;
    }
    if (stable && !holdFired && holdMs && now - pressedAt >= holdMs) { holdFired = true; return 2; }
    return 0;
  }
};

Button knobBtn{PIN_ENC_SW,   SYNC_HOLD_MS};
Button playBtn{PIN_KEY_PLAY, 0};
Button skipBtn{PIN_KEY_SKIP, SKIP_HOLD_MS};

int volume = 50;
bool muted = false;
uint32_t wordUntil = 0;
bool wordShowing = false;

void showWord(const uint8_t* w, uint32_t now) { display.setSegments(w); wordUntil = now + WORD_SHOW_MS; wordShowing = true; }

void refreshDisplay(uint32_t now) {
  static int lastShown = -2;
  if (now < wordUntil) return;
  bool dirty = wordShowing;
  wordShowing = false;
  int want = muted ? -1 : volume;
  if (!dirty && want == lastShown) return;
  lastShown = want;
  if (want == -1) display.setSegments(W_OFF);
  else display.showNumberDec(want, false);
}

void setup() {
  Serial.begin(115200);
  delay(50);
  pinMode(PIN_ENC_CLK, INPUT_PULLUP);
  pinMode(PIN_ENC_DT, INPUT_PULLUP);
  pinMode(PIN_ENC_SW, INPUT_PULLUP);
  pinMode(PIN_KEY_PLAY, INPUT_PULLUP);
  pinMode(PIN_KEY_SKIP, INPUT_PULLUP);
  encPrev = (digitalRead(PIN_ENC_CLK) << 1) | digitalRead(PIN_ENC_DT);
  attachInterrupt(digitalPinToInterrupt(PIN_ENC_CLK), onEncoderChange, CHANGE);
  attachInterrupt(digitalPinToInterrupt(PIN_ENC_DT), onEncoderChange, CHANGE);

  display.setBrightness(5, true);
  showWord(W_TEST, millis());
  Serial.println();
  Serial.println("Hardware test ready: turn the knob, press the knob, press Play and Skip.");
}

void loop() {
  uint32_t now = millis();

  int16_t d = takeDetents();
  while (d != 0) {
    if (d > 0) { volume = min(100, volume + VOL_STEP_PERCENT); d--; Serial.printf("Volume up   -> %d\n", volume); }
    else       { volume = max(0,   volume - VOL_STEP_PERCENT); d++; Serial.printf("Volume down -> %d\n", volume); }
    muted = false;
    wordUntil = 0;
  }

  switch (knobBtn.update(now)) {
    case 1: muted = !muted; Serial.println(muted ? "Mute" : "Unmute"); break;
    case 2: showWord(W_SYNC, now); volume = 0; muted = false; Serial.println("Sync: volume 0"); break;
  }
  if (playBtn.update(now) == 1) { showWord(W_PLAY, now); Serial.println("Play/Pause"); }
  switch (skipBtn.update(now)) {
    case 1: showWord(W_SKIP, now); Serial.println("Next"); break;
    case 2: showWord(W_PREV, now); Serial.println("Previous"); break;
  }

  refreshDisplay(now);
  delay(2);
}
