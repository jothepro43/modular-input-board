/*
  Modular Control Panel: modular I2C firmware
  ENGR 1101

  Board: Adafruit QT Py ESP32-S3 (No PSRAM). PCA9548 mux on the STEMMA QT port (Wire1).
  What it does:
    - Connects to an iPhone over Bluetooth using Apple Media Service (AMS). The panel is
      not a keyboard, so the iPhone keeps its on-screen keyboard.
    - Finds modules by scanning all 8 mux channels, at boot and every 2 s, so modules
      can sit on any port and can be moved while running.
    - I2C encoder (#5880): turn = volume up/down, press = mute, hold 3 s = settings menu.
    - OLED (#326): song title and artist, the Option A volume screen for 5 s after a change,
      MUTE, pairing, and the settings menu. Drawing is car_stereo_ui.h, an unchanged copy of
      firmware/display/car_stereo_ui.h. See firmware/display/DISPLAY_SPEC.md.
      With no screen attached, the onboard NeoPixel shows pairing status instead
      (blinking blue = waiting to pair, green = connected).
    - Key modules (ATtiny817) are detected and logged only. Play/Skip come later.

  Libraries: Adafruit seesaw, Adafruit SSD1306, Adafruit GFX, Adafruit NeoPixel.
  Use esp32 core 2.0.17. Send "C" over serial to forget all paired phones,
  "S" to dump the screen buffer (see tools/oled_screenshot.py), "R" to re-read volume and track.
  For testing the menu: "m" opens it, ">" "<" "P" act like a click up, click down and press
  but only while it is open, and "x" closes it. None of them can change the phone's volume.

  Limits: AMS is iPhone only and has no mute command, so mute lowers the volume to 0
  and unmute raises it back. The phone only accepts commands while it has a media player.
*/

#include <Wire.h>
#include <BLEDevice.h>
#include <BLESecurity.h>
#include <BLEServer.h>
#include <BLEClient.h>
#include "Adafruit_seesaw.h"
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_NeoPixel.h>
#include <Preferences.h>
#include "USB.h"
#include "USBHIDConsumerControl.h"
#include "car_stereo_ui.h"

extern "C" bool tud_mounted(void);  // true while a USB host has set the board up

// ---------------- I2C addresses ----------------
const uint8_t MUX_ADDR    = 0x70;
const uint8_t ENC_ADDR    = 0x36;
const uint8_t OLED_ADDR_A = 0x3C;
const uint8_t OLED_ADDR_B = 0x3D;  // the #326 on this board answers here
const uint8_t KEY_ADDR    = 0x49;
const uint8_t ENC_SWITCH  = 24;    // seesaw pin, per Adafruit's encoder_basic example

// ---------------- Tuning ----------------
const uint8_t  PHONE_VOL_STEPS  = 16;    // iPhone volume steps from 0 to full
const uint16_t DEBOUNCE_MS      = 25;
const uint16_t MENU_HOLD_MS     = 3000;  // hold the knob this long to open the settings menu
const uint16_t VOLUME_SHOW_MS   = 5000;  // how long the volume screen stays up after a change
const uint8_t  SONG_SCROLL_MS   = 60;    // ms per pixel of scrolling
const uint16_t SONG_PAUSE_MS    = 1500;  // rest at each end of a scroll
const uint16_t MENU_TIMEOUT_MS  = 10000; // no input for this long closes the menu and saves
const uint8_t  MENU_BLINK_MS    = 150;
const uint8_t  USB_VOL_STEP     = 6;     // estimate per step: USB hosts never report their volume
const uint32_t BT_OFF_MAGIC     = 0x54414143;
const uint16_t WORD_SHOW_MS     = 900;   // how long SYNC stays on screen
const uint16_t SCAN_EVERY_MS    = 2000;
const uint8_t  POLL_MS          = 10;

Adafruit_seesaw encoder(&Wire1);
Adafruit_SSD1306 oled(128, 64, &Wire1, -1);
Adafruit_NeoPixel statusPixel(1, PIN_NEOPIXEL, NEO_GRB + NEO_KHZ800);
USBHIDConsumerControl usbMedia;

// Survives a software restart but not a power cycle, as the BLUETOOTH > DISCONNECT item requires.
RTC_NOINIT_ATTR uint32_t btOffMark;
bool btOff = false;

// ---------------- Saved settings ----------------
enum LinkMode : uint8_t { LINK_AUTO, LINK_BT, LINK_USB };

struct Settings {
  uint8_t bright = 14;      // 1..20
  uint8_t step = 1;         // phone volume steps per knob click, 1..4
  bool    reverse = false;
  uint8_t link = LINK_BT;
  uint8_t saver = 2;        // index into SAVER_NAMES
  bool    bootAnim = true;
  bool    ghost = true;
} settings;

void loadSettings() {
  Preferences prefs;
  prefs.begin("taac", true);
  settings.bright = prefs.getUChar("bright", settings.bright);
  settings.step = prefs.getUChar("step", settings.step);
  settings.reverse = prefs.getBool("reverse", settings.reverse);
  settings.link = prefs.getUChar("link", settings.link);
  settings.saver = prefs.getUChar("saver", settings.saver);
  settings.bootAnim = prefs.getBool("boot", settings.bootAnim);
  settings.ghost = prefs.getBool("ghost", settings.ghost);
  prefs.end();
  carui::ghost() = settings.ghost;
}

void saveSettings() {
  Preferences prefs;
  prefs.begin("taac", false);
  prefs.putUChar("bright", settings.bright);
  prefs.putUChar("step", settings.step);
  prefs.putBool("reverse", settings.reverse);
  prefs.putUChar("link", settings.link);
  prefs.putUChar("saver", settings.saver);
  prefs.putBool("boot", settings.bootAnim);
  prefs.putBool("ghost", settings.ghost);
  prefs.end();
  Serial.println("Settings saved");
}

// ---------------- Bluetooth (Apple Media Service) ----------------
// UUIDs and IDs are from Apple's AMS specification.
const char* AMS_SERVICE       = "89D3502B-0F36-433A-8EF4-C502AD55F8DC";
const char* AMS_REMOTE_CMD    = "9B3C81D8-57B1-4A8A-B8DF-0E56F7CA51C2";
const char* AMS_ENTITY_UPDATE = "2F7CABCE-808D-411F-9A0C-BB92BA96C102";
const char* AMS_ENTITY_ATTR   = "C6B2F38C-23AB-46D8-A6AB-A3A870BBD5D7";
const uint8_t AMS_SERVICE_LE[16] = {0xDC, 0xF8, 0x55, 0xAD, 0x02, 0xC5, 0xF4, 0x8E,
                                    0x3A, 0x43, 0x36, 0x0F, 0x2B, 0x50, 0xD3, 0x89};
const uint8_t CMD_VOLUME_UP = 5;
const uint8_t CMD_VOLUME_DOWN = 6;
const uint8_t CMD_REREAD = 0xFF;  // not an AMS command: asks the task to read current values
const uint8_t ENTITY_PLAYER = 0;
const uint8_t ATTR_PLAYER_NAME = 0;
const uint8_t ATTR_VOLUME = 2;
const uint8_t ENTITY_TRACK = 2;
const uint8_t ATTR_ARTIST = 0;
const uint8_t ATTR_TITLE = 2;
const uint8_t FLAG_TRUNCATED = 1;
const uint16_t AMS_MTU = 185;  // room for long names in one notification

// Set by Bluetooth callbacks, read by the main loop and the AMS task.
volatile bool linked = false;     // phone has connected
volatile bool paired = false;     // secure pairing finished
volatile bool amsReady = false;   // subscribed to the phone's media service
volatile bool wasPaired = false;
volatile bool restartPending = false;
volatile int  phoneVolume = -1;   // 0..100, -1 = phone has no media player
volatile bool volumeChanged = false;
volatile bool trackChanged = false;
char trackTitle[64] = "";
char trackArtist[64] = "";
portMUX_TYPE trackLock = portMUX_INITIALIZER_UNLOCKED;
esp_bd_addr_t peerAddress;
esp_ble_addr_type_t peerAddressType;
QueueHandle_t amsQueue;
BLERemoteCharacteristic* remoteCmd = nullptr;
BLERemoteCharacteristic* entityUpdate = nullptr;
BLERemoteCharacteristic* entityAttr = nullptr;

class PairingLog : public BLESecurityCallbacks {
  uint32_t onPassKeyRequest() override { return 0; }
  void onPassKeyNotify(uint32_t) override {}
  bool onSecurityRequest() override { return true; }
  bool onConfirmPIN(uint32_t) override { return true; }
  void onAuthenticationComplete(esp_ble_auth_cmpl_t result) override {
    if (result.success) {
      Serial.printf("Pairing OK, auth mode 0x%02X\n", result.auth_mode);
      paired = wasPaired = true;
    } else {
      Serial.printf("Pairing FAILED, reason 0x%02X\n", result.fail_reason);
    }
  }
};

class LinkEvents : public BLEServerCallbacks {
  void onConnect(BLEServer*, esp_ble_gatts_cb_param_t* param) override {
    memcpy(peerAddress, param->connect.remote_bda, sizeof(esp_bd_addr_t));
    peerAddressType = param->connect.ble_addr_type;
    linked = true;
    Serial.println("Phone linked, asking for secure pairing");
    esp_ble_set_encryption(peerAddress, ESP_BLE_SEC_ENCRYPT);
  }
  void onDisconnect(BLEServer*) override {
    Serial.println("Phone disconnected");
    linked = paired = amsReady = false;
    restartPending = true;  // library calls can wait forever after a disconnect, so start clean
  }
};

// Keeps plain ASCII for the OLED font; each other character (UTF-8 sequence) becomes one '?'.
void storeText(char* out, size_t outSize, const uint8_t* in, size_t len, bool truncated) {
  char clean[64];
  size_t n = 0;
  for (size_t i = 0; i < len && n < sizeof(clean) - 1; i++) {
    if (in[i] < 0x80) clean[n++] = in[i];
    else if (in[i] >= 0xC0) clean[n++] = '?';
  }
  for (int i = 0; truncated && i < 3 && n < sizeof(clean) - 1; i++) clean[n++] = '.';
  clean[n] = 0;

  taskENTER_CRITICAL(&trackLock);
  bool changed = strncmp(out, clean, outSize) != 0;
  if (changed) strlcpy(out, clean, outSize);
  taskEXIT_CRITICAL(&trackLock);
  if (changed) trackChanged = true;
}

void onEntityUpdate(BLERemoteCharacteristic*, uint8_t* data, size_t len, bool) {
  if (len < 3) return;
  if (data[0] == ENTITY_TRACK) {
    bool truncated = data[2] & FLAG_TRUNCATED;
    if (data[1] == ATTR_TITLE) storeText(trackTitle, sizeof(trackTitle), data + 3, len - 3, truncated);
    if (data[1] == ATTR_ARTIST) storeText(trackArtist, sizeof(trackArtist), data + 3, len - 3, truncated);
    Serial.printf("Track attribute %u: '%.*s'%s\n", data[1], (int)(len - 3), (const char*)data + 3, truncated ? " (truncated)" : "");
    return;
  }
  if (data[0] != ENTITY_PLAYER) return;
  char value[24];
  size_t n = min(len - 3, sizeof(value) - 1);
  memcpy(value, data + 3, n);
  value[n] = 0;
  if (data[1] == ATTR_PLAYER_NAME) {
    Serial.printf("Player: '%s'\n", value);
    uint8_t cmd = CMD_REREAD;  // the phone may not send track details when the app changes
    xQueueSend(amsQueue, &cmd, 0);
  } else if (data[1] == ATTR_VOLUME) {
    phoneVolume = n ? (int)(atof(value) * 100 + 0.5) : -1;
    volumeChanged = true;
    Serial.printf("Phone volume: '%s'\n", value);
  }
}

void onRemoteCommands(BLERemoteCharacteristic*, uint8_t* data, size_t len, bool) {
  bool up = false;
  for (size_t i = 0; i < len; i++) up = up || data[i] == CMD_VOLUME_UP;
  Serial.printf("Phone offers %u commands, volume up %s\n", (unsigned)len, up ? "available" : "NOT available");
}

void subscribeVolume() {
  uint8_t request[] = {ENTITY_PLAYER, ATTR_PLAYER_NAME, ATTR_VOLUME};
  entityUpdate->writeValue(request, sizeof(request), true);
  uint8_t trackRequest[] = {ENTITY_TRACK, ATTR_ARTIST, ATTR_TITLE};
  entityUpdate->writeValue(trackRequest, sizeof(trackRequest), true);
}

std::string readAttribute(uint8_t entity, uint8_t attribute) {
  uint8_t request[] = {entity, attribute};
  entityAttr->writeValue(request, sizeof(request), true);
  return entityAttr->readValue();
}

// Subscribing only reports later changes, so ask for what is playing right now.
void readCurrentValues() {
  std::string title = readAttribute(ENTITY_TRACK, ATTR_TITLE);
  std::string artist = readAttribute(ENTITY_TRACK, ATTR_ARTIST);
  std::string volume = readAttribute(ENTITY_PLAYER, ATTR_VOLUME);
  storeText(trackTitle, sizeof(trackTitle), (const uint8_t*)title.data(), title.length(), false);
  storeText(trackArtist, sizeof(trackArtist), (const uint8_t*)artist.data(), artist.length(), false);
  phoneVolume = volume.empty() ? -1 : (int)(atof(volume.c_str()) * 100 + 0.5);
  volumeChanged = true;
  Serial.printf("Read: title '%s', artist '%s', volume '%s'\n", title.c_str(), artist.c_str(), volume.c_str());
}

bool connectAms() {
  BLEClient* client = BLEDevice::createClient();
  BLEAddress address(peerAddress);
  if (!client->connect(address, peerAddressType)) {
    Serial.println("AMS: could not open client connection");
    return false;
  }
  client->setMTU(AMS_MTU);
  BLERemoteService* service = client->getService(AMS_SERVICE);
  if (!service) {
    Serial.println("AMS: service not found on phone");
    return false;
  }
  remoteCmd = service->getCharacteristic(AMS_REMOTE_CMD);
  entityUpdate = service->getCharacteristic(AMS_ENTITY_UPDATE);
  entityAttr = service->getCharacteristic(AMS_ENTITY_ATTR);
  if (!remoteCmd || !entityUpdate || !entityAttr) {
    Serial.println("AMS: characteristics not found");
    return false;
  }
  remoteCmd->registerForNotify(onRemoteCommands);
  entityUpdate->registerForNotify(onEntityUpdate);
  subscribeVolume();
  readCurrentValues();
  Serial.printf("AMS: ready, MTU %u\n", client->getMTU());
  return true;
}

// Runs on its own task because these library calls block until the phone answers.
void amsTask(void*) {
  while (!paired) delay(50);
  delay(500);
  while (paired && !connectAms()) delay(2000);
  amsReady = paired;

  uint8_t cmd;
  while (true) {
    if (xQueueReceive(amsQueue, &cmd, portMAX_DELAY) != pdTRUE || !amsReady) continue;
    if (cmd == CMD_REREAD) readCurrentValues();
    else remoteCmd->writeValue(&cmd, 1, true);
  }
}

void sendCommand(uint8_t cmd) {
  if (xQueueSend(amsQueue, &cmd, 0) != pdTRUE) Serial.println("Command dropped: queue full");
}

void startBluetooth() {
  BLEDevice::init("Modular Panel");
  BLEDevice::setSecurityCallbacks(new PairingLog());

  BLESecurity security;
  security.setAuthenticationMode(ESP_LE_AUTH_REQ_SC_BOND);  // no MITM: the panel has no way to show or enter a code
  security.setCapability(ESP_IO_CAP_NONE);
  security.setKeySize(16);
  security.setInitEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);
  security.setRespEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);

  BLEServer* server = BLEDevice::createServer();
  server->setCallbacks(new LinkEvents());

  // Soliciting AMS is how an accessory asks an iPhone to connect and offer the service.
  BLEAdvertisementData advert;
  advert.setFlags(ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT);
  char header[2] = {17, ESP_BLE_AD_TYPE_128SOL_SRV_UUID};
  advert.addData(std::string(header, 2) + std::string((const char*)AMS_SERVICE_LE, 16));
  BLEAdvertisementData scanResponse;
  scanResponse.setName("Modular Panel");

  BLEAdvertising* advertising = BLEDevice::getAdvertising();
  advertising->setAdvertisementData(advert);
  advertising->setScanResponseData(scanResponse);
  if (!btOff) advertising->start();
}

void forgetAllPhones() {
  int count = esp_ble_get_bond_device_num();
  esp_ble_bond_dev_t* list = (esp_ble_bond_dev_t*)malloc(sizeof(esp_ble_bond_dev_t) * max(count, 1));
  if (list && esp_ble_get_bond_device_list(&count, list) == ESP_OK) {
    for (int i = 0; i < count; i++) esp_ble_remove_bond_device(list[i].bd_addr);
    Serial.printf("Forgot %d paired phone(s), restarting\n", count);
  }
  free(list);
  delay(300);
  ESP.restart();
}

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

Button knobBtn(MENU_HOLD_MS);

// ---------------- State ----------------
int8_t   encChannel = -1;       // -1 = not found
int8_t   oledChannel = -1;
uint8_t  keyChannels = 0;       // bitmask, logged only for now
bool     muxOk = false;
bool     wasConnected = false;
bool     muted = false;
uint8_t  mutedSteps = 0;        // volume steps to restore on unmute
uint8_t  moduleType[8] = {0};    // per mux channel, index into MODULE_NAMES
uint32_t songStart = 0;
bool     songScrolls = false;
uint32_t lastSongFrame = 0;
uint32_t volumeUntil = 0;       // while set, the volume screen is up
int      lastVolume = -1;
int      usbVolume = 50;        // estimate, used only over USB
const char* shownWord = nullptr;
uint32_t wordUntil = 0;
bool     menuOpen = false;
bool     menuEditing = false;
uint8_t  menuIndex = 0;
uint8_t  menuChoice = 0;        // current pick inside BLUETOOTH or MODULES
uint32_t menuLastInput = 0;
uint32_t lastMenuBlink = 0;
bool     lastPairBlink = false;
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

void applyBrightness() {
  if (oledChannel < 0 || !muxSelect(oledChannel)) return;
  oled.ssd1306_command(SSD1306_SETCONTRAST);
  oled.ssd1306_command(settings.bright * 12 + 15);
}

bool startOled(uint8_t ch, uint8_t addr) {
  if (!oled.begin(SSD1306_SWITCHCAPVCC, addr, true, false)) return false;
  oledChannel = ch;
  applyBrightness();
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
    moduleType[ch] = probe(ENC_ADDR) ? 1 : probe(KEY_ADDR) ? 2 : (probe(OLED_ADDR_A) || probe(OLED_ADDR_B)) ? 3 : 0;
  }
  if (keysNow != keyChannels) {
    keyChannels = keysNow;
    Serial.printf("Key modules on channel mask 0x%02X (not used yet)\n", keyChannels);
  }
}

// ---------------- Link (Bluetooth or USB) ----------------
bool usbActive() {
  if (settings.link == LINK_USB) return true;
  return settings.link == LINK_AUTO && tud_mounted();
}

int currentVolume() {
  return usbActive() ? usbVolume : phoneVolume;
}

void pressUsbKey(uint16_t key) {
  usbMedia.press(key);
  delay(10);
  usbMedia.release();
}

// direction is +1 or -1; one call is one knob click's worth of volume steps
void sendVolumeStep(int direction) {
  for (uint8_t i = 0; i < settings.step; i++) {
    if (usbActive()) {
      pressUsbKey(direction > 0 ? CONSUMER_CONTROL_VOLUME_INCREMENT : CONSUMER_CONTROL_VOLUME_DECREMENT);
      usbVolume = constrain(usbVolume + direction * USB_VOL_STEP, 0, 100);
    } else {
      sendCommand(direction > 0 ? CMD_VOLUME_UP : CMD_VOLUME_DOWN);
    }
  }
}

// ---------------- Screen / status ----------------
void showVolume() {
  volumeUntil = millis() + VOLUME_SHOW_MS;
  screenDirty = true;
}

void showWord(const char* word, uint32_t now) {
  shownWord = word;
  wordUntil = now + WORD_SHOW_MS;
  screenDirty = true;
}

// Scrolls from the start to the end of the text, rests, then starts over.
int scrollPx(int overflow, uint32_t elapsed) {
  if (overflow <= 0) return 0;
  uint32_t t = elapsed % (2 * SONG_PAUSE_MS + overflow * SONG_SCROLL_MS);
  return t < SONG_PAUSE_MS ? 0 : min(overflow, (int)((t - SONG_PAUSE_MS) / SONG_SCROLL_MS));
}

void drawSongLine(const char* text, int y, uint8_t size, uint32_t elapsed) {
  int overflow = (int)strlen(text) * 6 * size - 128;
  if (overflow > 0) songScrolls = true;
  carui::text(oled, text, -scrollPx(overflow, elapsed), y, size);
}

// Screen beyond the spec: the normal view when the volume isn't changing.
void drawSong(uint32_t now) {
  char title[64], artist[64];
  taskENTER_CRITICAL(&trackLock);
  strcpy(title, trackTitle);
  strcpy(artist, trackArtist);
  taskEXIT_CRITICAL(&trackLock);

  songScrolls = false;
  if (usbActive()) {
    carui::textC(oled, "USB", 16, 2);
  } else if (title[0]) {
    drawSongLine(title, 14, 2, now - songStart);
    drawSongLine(artist, 40, 1, now - songStart);
  } else {
    carui::textC(oled, phoneVolume < 0 ? "START MUSIC" : "NO SONG INFO", 28, 1);
  }
}

void drawVolumeScreen(bool bt) {
  int level = currentVolume();
  if (level < 0) {
    // Phone has no media player, so there is no volume to show
    carui::digits(oled, "----");
    carui::indicators(oled, {bt, false, false, false, true});
    carui::wedge(oled, 0);
  } else {
    carui::drawVolume(oled, level, bt);
  }
}

void drawMenuScreen(uint32_t now);

void drawScreen(bool connected) {
  if (oledChannel < 0 || !muxSelect(oledChannel)) return;
  uint32_t now = millis();
  bool bt = amsReady;

  oled.clearDisplay();
  if (shownWord) {
    carui::drawWord(oled, shownWord, max(currentVolume(), 0), bt);
  } else if (menuOpen) {
    drawMenuScreen(now);
  } else if (!connected) {
    if (btOff) carui::drawWord(oled, "BT OFF", 0, false);
    else carui::drawPairing(oled, now);
  } else if (muted) {
    carui::drawMuted(oled, bt);
  } else if (volumeUntil) {
    drawVolumeScreen(bt);
  } else {
    drawSong(now);
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

// ---------------- Settings menu ----------------
enum ItemKind : uint8_t { ITEM_ACTION, ITEM_RANGE, ITEM_OPTION, ITEM_CHOOSE, ITEM_LIST, ITEM_INFO };
enum ItemId : uint8_t { M_SYNC, M_BRIGHT, M_STEP, M_REVERSE, M_LINK, M_BLUETOOTH, M_MODULES,
                        M_SAVER, M_BOOT, M_GHOST, M_ABOUT, M_EXIT, MENU_COUNT };
struct MenuItem { const char* label; ItemKind kind; };
const MenuItem MENU[MENU_COUNT] = {
  {"SYNC VOL", ITEM_ACTION}, {"BRIGHT", ITEM_RANGE}, {"STEP", ITEM_RANGE}, {"REVERSE", ITEM_OPTION},
  {"LINK", ITEM_OPTION}, {"BLUETOOTH", ITEM_CHOOSE}, {"MODULES", ITEM_LIST}, {"SAVER", ITEM_OPTION},
  {"BOOT ANIM", ITEM_OPTION}, {"GHOST", ITEM_OPTION}, {"ABOUT", ITEM_INFO}, {"EXIT", ITEM_ACTION},
};
const char* const LINK_NAMES[] = {"AUTO", "BT", "USB"};
const char* const SAVER_NAMES[] = {"OFF", "1 MIN", "5 MIN", "10 MIN"};
const uint8_t SAVER_COUNT = 4;
const char* const MODULE_NAMES[] = {"EMPTY", "KNOB", "KEY", "OLED"};
const uint8_t BT_DISCONNECT = 0, BT_FORGET = 1, BT_CANCEL = 2, BT_CHOICES = 3;

const char* onOff(bool value) { return value ? "ON" : "OFF"; }

void menuValueText(char* out, size_t size) {
  switch (menuIndex) {
    case M_BRIGHT:  snprintf(out, size, "%u", settings.bright); break;
    case M_STEP:    snprintf(out, size, "%u STEP (%u%%)", settings.step, settings.step * 100 / PHONE_VOL_STEPS); break;
    case M_REVERSE: strlcpy(out, onOff(settings.reverse), size); break;
    case M_LINK:    strlcpy(out, LINK_NAMES[settings.link], size); break;
    case M_BLUETOOTH:
      if (!menuEditing) strlcpy(out, btOff ? "BT OFF" : amsReady ? "CONNECTED" : "NOT CONNECTED", size);
      else if (menuChoice == BT_DISCONNECT) strlcpy(out, btOff ? "TURN ON" : "DISCONNECT", size);
      else strlcpy(out, menuChoice == BT_FORGET ? "FORGET PHONE" : "CANCEL", size);
      break;
    case M_MODULES:
      if (menuEditing) {
        snprintf(out, size, "S%u %s", menuChoice + 1, MODULE_NAMES[moduleType[menuChoice]]);
      } else {
        uint8_t found = 0;
        for (uint8_t ch = 0; ch < 8; ch++) found += moduleType[ch] != 0;
        snprintf(out, size, "%u FOUND", found);
      }
      break;
    case M_SAVER:   strlcpy(out, SAVER_NAMES[settings.saver], size); break;
    case M_BOOT:    strlcpy(out, onOff(settings.bootAnim), size); break;
    case M_GHOST:   strlcpy(out, onOff(settings.ghost), size); break;
    case M_ABOUT:   strlcpy(out, "TAAC FW 0.1", size); break;
    default:        out[0] = 0; break;
  }
}

void drawMenuScreen(uint32_t now) {
  char value[16];
  menuValueText(value, sizeof(value));
  int8_t bar = menuIndex == M_BRIGHT ? settings.bright : -1;
  carui::drawMenu(oled, menuIndex, MENU_COUNT, MENU[menuIndex].label, value[0] ? value : nullptr, bar, menuEditing, now);
}

void openMenu(uint32_t now) {
  menuOpen = true;
  menuEditing = false;
  menuIndex = 0;
  menuLastInput = now;
  screenDirty = true;
  Serial.println("Menu open");
}

void closeMenu() {
  menuOpen = menuEditing = false;
  saveSettings();
  screenDirty = true;
  Serial.println("Menu closed");
}

void editValue(int direction) {
  switch (menuIndex) {
    case M_BRIGHT:
      settings.bright = constrain(settings.bright + direction, 1, 20);
      applyBrightness();
      break;
    case M_STEP:    settings.step = constrain(settings.step + direction, 1, 4); break;
    case M_REVERSE: settings.reverse = !settings.reverse; break;
    case M_LINK:    settings.link = (settings.link + 3 + direction) % 3; break;
    case M_BLUETOOTH: menuChoice = (menuChoice + BT_CHOICES + direction) % BT_CHOICES; break;
    case M_MODULES: menuChoice = (menuChoice + 8 + direction) % 8; break;
    case M_SAVER:   settings.saver = (settings.saver + SAVER_COUNT + direction) % SAVER_COUNT; break;
    case M_BOOT:    settings.bootAnim = !settings.bootAnim; break;
    case M_GHOST:
      settings.ghost = !settings.ghost;
      carui::ghost() = settings.ghost;
      break;
  }
}

void menuTurn(int direction, uint32_t now) {
  menuLastInput = now;
  if (menuEditing) editValue(direction);
  else menuIndex = (menuIndex + MENU_COUNT + direction) % MENU_COUNT;
  screenDirty = true;
}

void runBluetoothChoice() {
  if (menuChoice == BT_CANCEL) return;
  saveSettings();
  if (menuChoice == BT_FORGET) {
    btOffMark = 0;
    forgetAllPhones();
  }
  // Restarting is the clean way to drop the link; the mark decides whether to advertise afterwards.
  btOffMark = btOff ? 0 : BT_OFF_MAGIC;
  Serial.println(btOff ? "Bluetooth on, restarting" : "Bluetooth off, restarting");
  delay(100);
  ESP.restart();
}

void menuPress(uint32_t now) {
  menuLastInput = now;
  screenDirty = true;
  switch (MENU[menuIndex].kind) {
    case ITEM_ACTION:
      if (menuIndex == M_EXIT) {
        closeMenu();
      } else {
        sendCommand(CMD_REREAD);
        showWord("SYNC", now);
      }
      break;
    case ITEM_RANGE:
    case ITEM_OPTION:
      menuEditing = !menuEditing;
      break;
    case ITEM_CHOOSE:
      if (menuEditing) {
        menuEditing = false;
        runBluetoothChoice();
      } else {
        menuEditing = true;
        menuChoice = BT_CANCEL;  // a stray press must not drop the phone
      }
      break;
    case ITEM_LIST:
      menuEditing = !menuEditing;
      menuChoice = 0;
      break;
    case ITEM_INFO:
      break;
  }
}

// ---------------- Knob ----------------
void handleTurn(int32_t delta, bool connected, uint32_t now) {
  if (delta == 0) return;
  if (settings.reverse) delta = -delta;
  if (menuOpen) {
    while (delta != 0) {
      menuTurn(delta > 0 ? 1 : -1, now);
      delta += delta > 0 ? -1 : 1;
    }
    return;
  }
  if (!connected) {
    Serial.println("Turn ignored: not connected");
    return;
  }
  Serial.printf("Knob %+ld\n", (long)delta);
  while (delta != 0) {
    sendVolumeStep(delta > 0 ? 1 : -1);
    delta += delta > 0 ? -1 : 1;
  }
  muted = false;
  showVolume();
}

void handleButton(uint8_t action, bool connected, uint32_t now) {
  if (action == 0) return;
  if (menuOpen) {
    if (action == 1) menuPress(now);
    else closeMenu();           // no Play key yet, so holding the knob is the way back
    return;
  }
  if (action == 2) {
    openMenu(now);
    return;
  }
  if (!connected) {
    Serial.println("Press ignored: not connected");
    return;
  }
  if (usbActive()) {
    pressUsbKey(CONSUMER_CONTROL_MUTE);
  } else if (!muted) {
    // AMS has no mute command, so step the volume down and remember how far.
    mutedSteps = phoneVolume > 0 ? (phoneVolume * PHONE_VOL_STEPS + 50) / 100 : 0;
    for (uint8_t i = 0; i < mutedSteps; i++) sendCommand(CMD_VOLUME_DOWN);
  } else {
    for (uint8_t i = 0; i < mutedSteps; i++) sendCommand(CMD_VOLUME_UP);
  }
  muted = !muted;
  showVolume();
  Serial.println(muted ? "Mute" : "Unmute");
}

void pollEncoder(bool connected, uint32_t now) {
  if (encChannel < 0) return;
  muxSelect(encChannel);
  if (!probe(ENC_ADDR)) { loseEncoder(); return; }

  int32_t delta = encoder.getEncoderDelta();
  bool pressed = !encoder.digitalRead(ENC_SWITCH);

  // A module pulled out mid-read returns junk, which could look like a press or a big turn.
  if (!probe(ENC_ADDR)) { loseEncoder(); return; }

  handleTurn(delta, connected, now);
  handleButton(knobBtn.update(pressed, now), connected, now);
}

// Sends the screen buffer as hex, one line, for tools/oled_screenshot.py.
void dumpScreen() {
  if (oledChannel < 0) { Serial.println("NO SCREEN"); return; }
  const uint8_t* buffer = oled.getBuffer();
  String line = "SCREEN ";
  line.reserve(7 + 2048 + 1);
  for (int i = 0; i < 1024; i++) {
    if (buffer[i] < 16) line += '0';
    line += String(buffer[i], HEX);
  }
  Serial.println(line);
}

void setup() {
  usbMedia.begin();
  USB.begin();
  Serial.begin(115200);
  unsigned long t0 = millis();
  while (!Serial && millis() - t0 < 3000) delay(10);

  pinMode(NEOPIXEL_POWER, OUTPUT);
  digitalWrite(NEOPIXEL_POWER, NEOPIXEL_POWER_ON);
  statusPixel.begin();
  statusPixel.setBrightness(20);

  if (esp_reset_reason() == ESP_RST_POWERON) btOffMark = 0;
  btOff = btOffMark == BT_OFF_MAGIC;
  loadSettings();

  Wire1.begin(SDA1, SCL1);
  scanModules();

  amsQueue = xQueueCreate(32, sizeof(uint8_t));
  startBluetooth();
  xTaskCreate(amsTask, "ams", 8192, nullptr, 1, nullptr);
  Serial.println("Modular Panel ready. Pair with \"Modular Panel\" in your iPhone's Bluetooth settings.");
}

void loop() {
  uint32_t now = millis();
  bool connected = amsReady || usbActive();

  if (restartPending) {
    delay(200);
    if (wasPaired) ESP.restart();
    restartPending = false;
    BLEDevice::getAdvertising()->start();
  }
  if (Serial.available()) {
    char command = Serial.read();
    if (command == 'C') forgetAllPhones();
    if (command == 'S') dumpScreen();
    if (command == 'R') sendCommand(CMD_REREAD);
    if (command == 'm' && !menuOpen) openMenu(now);
    if (command == 'x' && menuOpen) closeMenu();
    if (command == '>' && menuOpen) menuTurn(1, now);
    if (command == '<' && menuOpen) menuTurn(-1, now);
    if (command == 'P' && menuOpen) menuPress(now);
  }
  if (volumeChanged) {
    volumeChanged = false;
    if (phoneVolume != lastVolume) { lastVolume = phoneVolume; showVolume(); }
  }
  if (volumeUntil && now >= volumeUntil) { volumeUntil = 0; screenDirty = true; }
  if (trackChanged) {
    trackChanged = false;
    songStart = now;
    screenDirty = true;
  }
  if (songScrolls && !volumeUntil && now - lastSongFrame >= SONG_SCROLL_MS) { lastSongFrame = now; screenDirty = true; }
  if (shownWord && now >= wordUntil) { shownWord = nullptr; screenDirty = true; }
  if (menuOpen && now - menuLastInput >= MENU_TIMEOUT_MS) {
    Serial.println("Menu auto-exit");
    closeMenu();
  }
  if (menuOpen && menuEditing && now - lastMenuBlink >= MENU_BLINK_MS) { lastMenuBlink = now; screenDirty = true; }
  bool pairBlink = (now / 500) % 2;
  if (!connected && !btOff && pairBlink != lastPairBlink) { lastPairBlink = pairBlink; screenDirty = true; }

  if (connected != wasConnected) {
    wasConnected = connected;
    screenDirty = true;
    Serial.println(connected ? "Bluetooth connected" : "Bluetooth disconnected");
  }

  if (now - lastScan >= SCAN_EVERY_MS) {
    lastScan = now;
    scanModules();
    Serial.printf("[alive] mux=%s encoder=%d oled=%d keys=0x%02X linked=%d paired=%d ams=%d vol=%d%s\n", muxOk ? "ok" : "MISSING",
                  encChannel, oledChannel, keyChannels, linked, paired, amsReady, phoneVolume, muted ? " muted" : "");
  }

  pollEncoder(connected, now);

  if (screenDirty) { screenDirty = false; drawScreen(connected); }
  updatePixel(connected, now);

  delay(POLL_MS);
}
