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
    - Key modules (NeoKey socket + ATtiny817): up to two, Play/Pause and Skip (tap = next
      track, hold 0.7 s = previous track). In the menu, Play goes back.
      Each key remembers its own role in the ATtiny's EEPROM, so the role follows the key to
      any port. A key with no role yet takes the free one (Play first) and keeps it.
      Menu > SWAP KEYS, or "K" over serial, swaps the two roles.
      To tell them apart: key LED is dim green = Play, dim blue = Skip (white while pressed),
      and Menu > MODULES lists each port as PLAY or SKIP.

  Libraries: Adafruit seesaw, Adafruit SSD1306, Adafruit GFX, Adafruit NeoPixel.
  Use esp32 core 2.0.17. Send "C" over serial to forget all paired phones,
  "S" to dump the screen buffer (see tools/oled_screenshot.py), "R" to re-read volume and track,
  "K" to swap the Play and Skip keys.
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
#include "seesaw_neopixel.h"
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
const uint8_t KEY_SWITCH  = 18;    // seesaw pin on the ATtiny817, pressed reads LOW (firmware/key_module_test)
const uint8_t KEY_PIXEL   = 19;    // seesaw pin for the NeoKey's NeoPixel
const uint8_t KEY_ROLE_EEPROM = 0;    // byte in the key module's own EEPROM that holds its role
const uint8_t KEY_ROLE_MARK   = 0xA0; // stored value is this mark plus the role (0 Play, 1 Skip)

// ---------------- Tuning ----------------
const uint8_t  PHONE_VOL_STEPS  = 16;    // iPhone volume steps from 0 to full
const uint16_t DEBOUNCE_MS      = 25;
const uint16_t MENU_HOLD_MS     = 3000;  // hold the knob this long to open the settings menu
const uint16_t SKIP_HOLD_MS     = 700;   // hold Skip this long for previous track
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
// seesaw_NeoPixel is also an Adafruit_seesaw, so one object per key reads the switch and drives the LED.
const uint8_t KEY_PLAY = 0, KEY_SKIP = 1, KEY_COUNT = 2;
seesaw_NeoPixel keyBoard[KEY_COUNT] = {
  seesaw_NeoPixel(1, KEY_PIXEL, NEO_GRB + NEO_KHZ800, &Wire1),
  seesaw_NeoPixel(1, KEY_PIXEL, NEO_GRB + NEO_KHZ800, &Wire1),
};
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
const uint8_t CMD_PLAY_PAUSE = 2;
const uint8_t CMD_NEXT_TRACK = 3;
const uint8_t CMD_PREV_TRACK = 4;
const uint8_t CMD_VOLUME_UP = 5;
const uint8_t CMD_VOLUME_DOWN = 6;
const uint8_t CMD_REREAD = 0xFF;  // not an AMS command: asks the task to read current values
const uint8_t ENTITY_PLAYER = 0;
const uint8_t ATTR_PLAYER_NAME = 0;
const uint8_t ATTR_PLAYBACK = 1;   // "state,rate,elapsed"; state 1 = playing
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
volatile int  playState = -1;     // 1 playing, 0 paused, -1 unknown
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
  } else if (data[1] == ATTR_PLAYBACK) {
    playState = n ? (value[0] == '1') : -1;
    Serial.printf("Playback: '%s'\n", value);
  } else if (data[1] == ATTR_VOLUME) {
    phoneVolume = n ? (int)(atof(value) * 100 + 0.5) : -1;
    volumeChanged = true;
    Serial.printf("Phone volume: '%s'\n", value);
  }
}

void onRemoteCommands(BLERemoteCharacteristic*, uint8_t* data, size_t len, bool) {
  bool up = false;
  for (size_t i = 0; i < len; i++) up = up || data[i] == CMD_VOLUME_UP;
  Serial.printf("Phone offers %u commands, volume up %s:", (unsigned)len, up ? "available" : "NOT available");
  for (size_t i = 0; i < len; i++) Serial.printf(" %u", data[i]);
  Serial.println();
}

void subscribeVolume() {
  uint8_t request[] = {ENTITY_PLAYER, ATTR_PLAYER_NAME, ATTR_PLAYBACK, ATTR_VOLUME};
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
  // iPhones list a new accessory in Bluetooth settings only for some device types, so this also
  // advertises HID, as Espressif's AMS example does. The panel has no HID service.
  advert.setAppearance(ESP_BLE_APPEARANCE_GENERIC_HID);
  advert.setCompleteServices(BLEUUID((uint16_t)0x1812));
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
Adafruit_seesaw keyProbe(&Wire1);  // reads a key's stored role without resetting the module
Button keyBtn[KEY_COUNT] = {Button(0), Button(SKIP_HOLD_MS)};  // Play has no hold action

// ---------------- State ----------------
int8_t   encChannel = -1;       // -1 = not found
int8_t   oledChannel = -1;
uint8_t  keyChannels = 0;       // bitmask of mux channels with a key module
int8_t   keyChannel[KEY_COUNT] = {-1, -1};  // per role, -1 = not found
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

void setKeyColor(uint8_t role, bool pressed) {
  seesaw_NeoPixel &k = keyBoard[role];
  uint32_t idle = role == KEY_PLAY ? k.Color(0, 20, 0) : k.Color(0, 0, 20);
  k.setPixelColor(0, pressed ? k.Color(255, 255, 255) : idle);
  k.show();
}

void loseKey(uint8_t role) {
  Serial.printf("%s key lost from channel %d\n", role == KEY_PLAY ? "Play" : "Skip", keyChannel[role]);
  keyChannel[role] = -1;
  keyBtn[role].clear();
}

const char* keyName(uint8_t role) { return role == KEY_PLAY ? "PLAY" : "SKIP"; }

// Returns the role stored on the key module on this channel, or -1 if it has none yet.
int8_t readKeyRole(uint8_t ch) {
  muxSelect(ch);
  if (!keyProbe.begin(KEY_ADDR, -1, false)) return -1;
  uint8_t value = keyProbe.EEPROMRead8(KEY_ROLE_EEPROM);
  return (value & 0xFE) == KEY_ROLE_MARK ? (value & 1) : -1;
}

// Stores a role on the key module that currently has `role`'s object and channel.
void writeKeyRole(uint8_t role, uint8_t newRole) {
  muxSelect(keyChannel[role]);
  keyBoard[role].EEPROMWrite8(KEY_ROLE_EEPROM, KEY_ROLE_MARK | newRole);
  delay(20);  // EEPROM write time
  bool ok = keyBoard[role].EEPROMRead8(KEY_ROLE_EEPROM) == (KEY_ROLE_MARK | newRole);
  Serial.printf("Key on channel %d labelled %s: %s\n", keyChannel[role], keyName(newRole), ok ? "saved" : "SAVE FAILED");
}

bool startKey(uint8_t role, uint8_t ch) {
  muxSelect(ch);
  if (!keyBoard[role].begin(KEY_ADDR)) {
    Serial.printf("Key on channel %u did not start\n", ch);
    return false;
  }
  keyBoard[role].pinMode(KEY_SWITCH, INPUT_PULLUP);
  keyBoard[role].setBrightness(255);
  keyChannel[role] = ch;
  setKeyColor(role, false);
  // A key held while it is plugged in must not count as a press.
  bool held = !keyBoard[role].digitalRead(KEY_SWITCH);
  keyBtn[role].stable = keyBtn[role].lastRaw = keyBtn[role].holdFired = held;
  Serial.printf("%s key on channel %u\n", keyName(role), ch);
  return true;
}

// Each key carries its role. Keys that already have one are placed first; a key with no
// role (or the same role as another key) then takes whichever role is still free.
void assignKeys(uint8_t mask) {
  for (uint8_t role = 0; role < KEY_COUNT; role++)
    if (keyChannel[role] >= 0 && !(mask & (1 << keyChannel[role]))) loseKey(role);

  for (uint8_t pass = 0; pass < 2; pass++) {
    for (uint8_t ch = 0; ch < 8; ch++) {
      if (!(mask & (1 << ch)) || ch == keyChannel[KEY_PLAY] || ch == keyChannel[KEY_SKIP]) continue;
      if (keyChannel[KEY_PLAY] >= 0 && keyChannel[KEY_SKIP] >= 0) return;  // both roles filled
      int8_t stored = readKeyRole(ch);
      bool useStored = stored >= 0 && keyChannel[stored] < 0;
      if (pass == 0 && !useStored) continue;
      uint8_t role = useStored ? stored : (keyChannel[KEY_PLAY] < 0 ? KEY_PLAY : KEY_SKIP);
      if (!startKey(role, ch)) continue;
      if (stored < 0) writeKeyRole(role, role);
      else if (stored != role) Serial.printf("Two keys are labelled %s; channel %u acts as %s for now\n", keyName(stored), ch, keyName(role));
    }
  }
}

// Swaps Play and Skip. With one key connected, it just changes that key's role.
void swapKeys() {
  if (keyChannel[KEY_PLAY] < 0 && keyChannel[KEY_SKIP] < 0) {
    Serial.println("Swap keys: no key connected");
    return;
  }
  for (uint8_t role = 0; role < KEY_COUNT; role++)
    if (keyChannel[role] >= 0) writeKeyRole(role, role == KEY_PLAY ? KEY_SKIP : KEY_PLAY);
  for (uint8_t role = 0; role < KEY_COUNT; role++) {
    keyChannel[role] = -1;
    keyBtn[role].clear();
  }
  assignKeys(keyChannels);  // picks the new labels back up
}

void scanModules() {
  muxOk = muxSelect(0);
  if (!muxOk) {
    if (encChannel >= 0 || oledChannel >= 0) Serial.println("Mux lost");
    Serial.println("Mux NOT found at 0x70");
    encChannel = oledChannel = -1;
    keyChannels = 0;
    keyChannel[KEY_PLAY] = keyChannel[KEY_SKIP] = -1;
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
    Serial.printf("Key modules on channel mask 0x%02X\n", keyChannels);
  }
  assignKeys(keysNow);  // also retries a key that failed to start
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
enum ItemId : uint8_t { M_SYNC, M_BRIGHT, M_STEP, M_REVERSE, M_LINK, M_BLUETOOTH, M_MODULES, M_KEYS,
                        M_SAVER, M_BOOT, M_GHOST, M_ABOUT, M_EXIT, MENU_COUNT };
struct MenuItem { const char* label; ItemKind kind; };
const MenuItem MENU[MENU_COUNT] = {
  {"SYNC VOL", ITEM_ACTION}, {"BRIGHT", ITEM_RANGE}, {"STEP", ITEM_RANGE}, {"REVERSE", ITEM_OPTION},
  {"LINK", ITEM_OPTION}, {"BLUETOOTH", ITEM_CHOOSE}, {"MODULES", ITEM_LIST}, {"SWAP KEYS", ITEM_ACTION},
  {"SAVER", ITEM_OPTION},
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
        const char* name = MODULE_NAMES[moduleType[menuChoice]];
        for (uint8_t role = 0; role < KEY_COUNT; role++)
          if (keyChannel[role] == menuChoice) name = keyName(role);
        snprintf(out, size, "S%u %s", menuChoice + 1, name);
      } else {
        uint8_t found = 0;
        for (uint8_t ch = 0; ch < 8; ch++) found += moduleType[ch] != 0;
        snprintf(out, size, "%u FOUND", found);
      }
      break;
    case M_KEYS: {
      char play[4] = "--", skip[4] = "--";
      if (keyChannel[KEY_PLAY] >= 0) snprintf(play, sizeof(play), "S%d", keyChannel[KEY_PLAY] + 1);
      if (keyChannel[KEY_SKIP] >= 0) snprintf(skip, sizeof(skip), "S%d", keyChannel[KEY_SKIP] + 1);
      snprintf(out, size, "PLAY %s SKIP %s", play, skip);
      break;
    }
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
      } else if (menuIndex == M_KEYS) {
        swapKeys();
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
    else closeMenu();           // holding the knob also goes back, for panels with no Play key
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

// ---------------- Keys ----------------
void sendMedia(uint8_t amsCmd, uint16_t usbKey) {
  if (usbActive()) pressUsbKey(usbKey);
  else sendCommand(amsCmd);
}

void handleKey(uint8_t role, uint8_t action, bool connected, uint32_t now) {
  if (action == 0) return;
  if (menuOpen) {
    // Play is the back key: leave the value being edited, or leave the menu.
    if (role == KEY_PLAY) {
      menuLastInput = now;
      if (menuEditing) { menuEditing = false; screenDirty = true; }
      else closeMenu();
    }
    return;
  }
  if (!connected) {
    Serial.println("Key ignored: not connected");
    return;
  }
  if (role == KEY_PLAY) {
    bool wasPlaying = !usbActive() && playState == 1;
    sendMedia(CMD_PLAY_PAUSE, CONSUMER_CONTROL_PLAY_PAUSE);
    showWord(wasPlaying ? "PAUSE" : "PLAY", now);
    Serial.println("Key: play/pause");
  } else if (action == 1) {
    sendMedia(CMD_NEXT_TRACK, CONSUMER_CONTROL_SCAN_NEXT);
    showWord("SEEK>", now);
    Serial.println("Key: next track");
  } else {
    sendMedia(CMD_PREV_TRACK, CONSUMER_CONTROL_SCAN_PREVIOUS);
    showWord("<SEEK", now);
    Serial.println("Key: previous track");
  }
}

void pollKeys(bool connected, uint32_t now) {
  for (uint8_t role = 0; role < KEY_COUNT; role++) {
    if (keyChannel[role] < 0) continue;
    muxSelect(keyChannel[role]);
    if (!probe(KEY_ADDR)) { loseKey(role); continue; }
    bool pressed = !keyBoard[role].digitalRead(KEY_SWITCH);
    // A module pulled out mid-read returns junk, which could look like a press.
    if (!probe(KEY_ADDR)) { loseKey(role); continue; }

    bool before = keyBtn[role].stable;
    uint8_t action = keyBtn[role].update(pressed, now);
    if (keyBtn[role].stable != before) setKeyColor(role, keyBtn[role].stable);
    handleKey(role, action, connected, now);
  }
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
    if (command == 'K') swapKeys();
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
    Serial.printf("[alive] mux=%s encoder=%d oled=%d play=%d skip=%d linked=%d paired=%d ams=%d vol=%d%s\n", muxOk ? "ok" : "MISSING",
                  encChannel, oledChannel, keyChannel[KEY_PLAY], keyChannel[KEY_SKIP], linked, paired, amsReady, phoneVolume, muted ? " muted" : "");
  }

  pollEncoder(connected, now);
  pollKeys(connected, now);

  if (screenDirty) { screenDirty = false; drawScreen(connected); }
  updatePixel(connected, now);

  delay(POLL_MS);
}
