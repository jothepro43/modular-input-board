// Bring-up step 4: show the encoder's count on the OLED, each on its own PCA9548 channel.
#include <Wire.h>
#include "Adafruit_seesaw.h"
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

const uint8_t MUX_ADDR = 0x70;
const uint8_t ENC_CHANNEL = 0;
const uint8_t ENC_ADDR = 0x36;
const uint8_t ENC_SWITCH = 24;
const uint8_t OLED_CHANNEL = 1;
const uint8_t OLED_ADDR = 0x3D;  // found by the step 2 scan on this board

Adafruit_seesaw ss(&Wire1);
Adafruit_SSD1306 oled(128, 64, &Wire1, -1);
bool encoderOk = false;
bool oledOk = false;
int32_t count = 0;
bool pressed = false;
unsigned long lastHeartbeat = 0;

bool muxSelect(uint8_t ch) {
  Wire1.beginTransmission(MUX_ADDR);
  Wire1.write(1 << ch);
  return Wire1.endTransmission() == 0;
}

void draw() {
  if (!oledOk) return;
  muxSelect(OLED_CHANNEL);
  oled.clearDisplay();
  oled.setTextColor(SSD1306_WHITE);
  oled.setTextSize(1);
  oled.setCursor(0, 0);
  oled.print(pressed ? "PRESS" : "COUNT");
  oled.setTextSize(4);
  oled.setCursor(0, 24);
  oled.print(count);
  oled.display();
}

void setup() {
  Serial.begin(115200);
  unsigned long t0 = millis();
  while (!Serial && millis() - t0 < 3000) delay(10);
  Wire1.begin(SDA1, SCL1);

  muxSelect(ENC_CHANNEL);
  encoderOk = ss.begin(ENC_ADDR);
  if (encoderOk) {
    ss.pinMode(ENC_SWITCH, INPUT_PULLUP);
    ss.getEncoderDelta();
  }

  muxSelect(OLED_CHANNEL);
  oledOk = oled.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR, true, false);
  draw();
}

void loop() {
  if (millis() - lastHeartbeat > 5000) {
    lastHeartbeat = millis();
    Serial.printf("[alive] encoder=%s oled=%s count=%ld\n", encoderOk ? "ok" : "MISSING",
                  oledOk ? "ok" : "MISSING", (long)count);
  }
  if (!encoderOk) { delay(50); return; }

  muxSelect(ENC_CHANNEL);
  int32_t delta = ss.getEncoderDelta();
  bool nowPressed = !ss.digitalRead(ENC_SWITCH);

  if (delta != 0 || nowPressed != pressed) {
    count += delta;
    pressed = nowPressed;
    Serial.printf("count %ld  %s\n", (long)count, pressed ? "PRESS" : "");
    draw();
  }

  delay(10);
}
