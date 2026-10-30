// Bring-up step 2: I2C scan across all 8 PCA9548 channels.
// QT Py ESP32-S3 STEMMA QT port is Wire1 (SDA1 = 41, SCL1 = 40), per the Adafruit pinouts page.
#include <Wire.h>

const uint8_t MUX_ADDR = 0x70;

// Writes the mux control register: one bit per channel, 0 = all channels closed.
bool muxWrite(uint8_t mask) {
  Wire1.beginTransmission(MUX_ADDR);
  Wire1.write(mask);
  return Wire1.endTransmission() == 0;
}

const char* guessName(uint8_t addr) {
  switch (addr) {
    case 0x36: return "seesaw encoder (#5880)";
    case 0x3C:
    case 0x3D: return "OLED (#326)";
    case 0x49: return "ATtiny817 seesaw (#5233)";
    default:   return "unknown";
  }
}

// Lists every responding address on the currently open channel. Returns the count.
int scanOpenChannel() {
  int found = 0;
  for (uint8_t addr = 1; addr < 127; addr++) {
    if (addr == MUX_ADDR) continue;
    Wire1.beginTransmission(addr);
    if (Wire1.endTransmission() == 0) {
      Serial.printf("  0x%02X  %s\n", addr, guessName(addr));
      found++;
    }
  }
  return found;
}

void setup() {
  Serial.begin(115200);
  unsigned long t0 = millis();
  while (!Serial && millis() - t0 < 3000) delay(10);
  Wire1.begin(SDA1, SCL1);
  Wire1.setClock(100000);
}

void loop() {
  Serial.println("\n=== I2C scan (Wire1) ===");

  if (!muxWrite(0)) {
    Serial.println("Mux NOT found at 0x70. Check the STEMMA QT cable to the PCA9548.");
    delay(5000);
    return;
  }
  Serial.println("Mux found at 0x70");

  Serial.println("All channels closed (should list nothing):");
  if (scanOpenChannel() == 0) Serial.println("  none");

  for (uint8_t ch = 0; ch < 8; ch++) {
    muxWrite(1 << ch);
    Serial.printf("Channel %u:\n", ch);
    if (scanOpenChannel() == 0) Serial.println("  none");
  }
  muxWrite(0);

  delay(5000);
}
