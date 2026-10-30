// Bring-up step 3: read the seesaw encoder (#5880) through PCA9548 channel 0.
// Prints turn direction, running count and button presses at 115200.
#include <Wire.h>
#include "Adafruit_seesaw.h"

const uint8_t MUX_ADDR = 0x70;
const uint8_t ENC_CHANNEL = 0;
const uint8_t ENC_ADDR = 0x36;
const uint8_t ENC_SWITCH = 24;  // seesaw pin, per Adafruit's encoder_basic example

Adafruit_seesaw ss(&Wire1);
bool encoderOk = false;
int32_t count = 0;
bool lastPressed = false;
unsigned long lastHeartbeat = 0;

bool muxSelect(uint8_t ch) {
  Wire1.beginTransmission(MUX_ADDR);
  Wire1.write(1 << ch);
  return Wire1.endTransmission() == 0;
}

void setup() {
  Serial.begin(115200);
  unsigned long t0 = millis();
  while (!Serial && millis() - t0 < 3000) delay(10);
  Wire1.begin(SDA1, SCL1);

  if (!muxSelect(ENC_CHANNEL)) {
    Serial.println("Mux NOT found at 0x70");
    return;
  }
  if (!ss.begin(ENC_ADDR)) {
    Serial.println("Encoder NOT found at 0x36 on channel 0");
    return;
  }
  encoderOk = true;
  Serial.printf("Encoder found, product id %u\n", (unsigned)((ss.getVersion() >> 16) & 0xFFFF));
  ss.pinMode(ENC_SWITCH, INPUT_PULLUP);
  ss.getEncoderDelta();  // clear anything counted before now
}

void loop() {
  if (millis() - lastHeartbeat > 5000) {
    lastHeartbeat = millis();
    Serial.printf("[alive] encoder=%s count=%ld\n", encoderOk ? "ok" : "MISSING", (long)count);
  }
  if (!encoderOk) { delay(50); return; }

  int32_t delta = ss.getEncoderDelta();
  if (delta != 0) {
    count += delta;
    Serial.printf("raw delta %+ld  count %ld\n", (long)delta, (long)count);
  }

  bool pressed = !ss.digitalRead(ENC_SWITCH);
  if (pressed != lastPressed) {
    lastPressed = pressed;
    Serial.println(pressed ? "BUTTON DOWN" : "BUTTON UP");
  }

  delay(10);
}
