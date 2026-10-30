// car_stereo_ui.h - "Option A: car stereo" screen design for the 0.96" 128x64 SSD1306 OLED (Adafruit #326).
// Ported 1:1 from the canvas design (OLED Screen Animations, artboards Main / Screens / Frames).
//
// Usage (caller owns the display and the mux channel select):
//   display.clearDisplay();
//   carui::drawVolume(display, 50, true);   // or drawMuted / drawWord / drawPairing / drawStandby / drawBoot
//   display.display();
//
// Differences from the on-screen mockup (hardware is 1-bit white):
//   - No glow, no teal. Unlit "ghost" segments are optional: ghost() = true draws them as a sparse dot pattern.
//   - Unlit wedge bars are drawn as outlines so the empty bar still reads.
//   - Text uses Adafruit GFX's built-in 5x7 font (6 px advance), same metrics as the design.
#pragma once
#include <Arduino.h>
#include <Adafruit_GFX.h>

namespace carui {

// Faint unlit segments (sparse dots). Runtime setting so the menu's GHOST item can toggle it: carui::ghost() = false;
inline bool &ghost() { static bool g = true; return g; }
static const uint16_t ON = 1;                   // SSD1306_WHITE
static const int16_t DIGIT_X[4] = {4, 20, 36, 52};
static const int16_t DIGIT_Y = 6, DIGIT_W = 12, DIGIT_H = 30, SEG_T = 3;

// ---------- primitives ----------
inline void dither(Adafruit_GFX &d, int16_t x, int16_t y, int16_t w, int16_t h) {
  for (int16_t j = 0; j < h; j++)
    for (int16_t i = 0; i < w; i++)
      if (((x + i) % 3 == 0) && ((y + j) % 3 == 0)) d.drawPixel(x + i, y + j, ON);
}
inline void text(Adafruit_GFX &d, const char *s, int16_t x, int16_t y, uint8_t size) {
  d.setTextColor(ON); d.setTextSize(size); d.setTextWrap(false); d.setCursor(x, y); d.print(s);
}
inline int16_t textW(const char *s, uint8_t size) { return (int16_t)strlen(s) * 6 * size - size; }
inline void textC(Adafruit_GFX &d, const char *s, int16_t y, uint8_t size) { text(d, s, (128 - textW(s, size)) / 2, y, size); }

// ---------- 7-segment digits ----------
// bit 0..6 = segments a..g
inline uint8_t segMask(char c) {
  switch (c) {
    case '0': return 0x3F; case '1': return 0x06; case '2': return 0x5B; case '3': return 0x4F;
    case '4': return 0x66; case '5': return 0x6D; case '6': return 0x7D; case '7': return 0x07;
    case '8': return 0x7F; case '9': return 0x6F; case '-': return 0x40;
    case 't': return 0x78; case 'A': return 0x77; case 'C': return 0x39;
    default:  return 0x00;
  }
}
inline void segDigit(Adafruit_GFX &d, char c, int16_t x) {
  const int16_t y = DIGIT_Y, w = DIGIT_W, h = DIGIT_H, t = SEG_T, hh = 15;
  const int16_t r[7][4] = {
    {int16_t(x + 1), y, int16_t(w - 2), t},                                   // a
    {int16_t(x + w - t), int16_t(y + 1), t, int16_t(hh - 1)},                 // b
    {int16_t(x + w - t), int16_t(y + hh + 1), t, int16_t(h - hh - 2)},        // c
    {int16_t(x + 1), int16_t(y + h - t), int16_t(w - 2), t},                  // d
    {x, int16_t(y + hh + 1), t, int16_t(h - hh - 2)},                         // e
    {x, int16_t(y + 1), t, int16_t(hh - 1)},                                  // f
    {int16_t(x + 1), int16_t(y + hh - 1), int16_t(w - 2), t}                  // g
  };
  uint8_t m = segMask(c);
  for (uint8_t i = 0; i < 7; i++) {
    if (m & (1 << i)) d.fillRect(r[i][0], r[i][1], r[i][2], r[i][3], ON);
    else if (ghost()) dither(d, r[i][0], r[i][1], r[i][2], r[i][3]);
  }
}
// Right-aligned in 4 digit positions, e.g. "50" -> "  50".
inline void digits(Adafruit_GFX &d, const char *s) {
  char buf[5] = "    ";
  size_t n = strlen(s); if (n > 4) n = 4;
  memcpy(buf + 4 - n, s, n);
  for (uint8_t i = 0; i < 4; i++) segDigit(d, buf[i], DIGIT_X[i]);
}

// ---------- indicators + wedge ----------
struct Lamps { bool bt, mute, seek, play, vol; };
inline void indicators(Adafruit_GFX &d, Lamps L) {
  if (L.bt)   text(d, "BT",   74, 6, 1);
  if (L.mute) text(d, "MUTE", 74, 15, 1);
  if (L.seek) text(d, "SEEK", 74, 24, 1);
  if (L.play) text(d, "PLAY", 74, 33, 1);
  if (L.vol)  text(d, "VOL", 107, 6, 1);
}
// n = lit bars 0..20 (volume / 5). Bars grow from 3 px to 14 px tall.
inline void wedge(Adafruit_GFX &d, uint8_t n) {
  for (uint8_t i = 0; i < 20; i++) {
    int16_t h = 3 + (i * 11 + 9) / 19, x = 4 + i * 6, y = 61 - h;
    if (i < n) d.fillRect(x, y, 4, h, ON); else d.drawRect(x, y, 4, h, ON);
  }
}

// ---------- screens ----------
inline void drawVolume(Adafruit_GFX &d, int vol, bool bt) {
  char b[5]; snprintf(b, sizeof b, "%d", vol);
  digits(d, b); indicators(d, {bt, false, false, false, true}); wedge(d, (vol + 2) / 5);
}
inline void drawMuted(Adafruit_GFX &d, bool bt) { digits(d, "----"); indicators(d, {bt, true, false, false, false}); wedge(d, 0); }
// word: "PLAY", "SEEK>", "<SEEK", "SYNC" (max 5 chars at size 2)
inline void drawWord(Adafruit_GFX &d, const char *word, int vol, bool bt) {
  bool seek = word[0] == '<' || strchr(word, '>') != nullptr;
  bool play = strcmp(word, "PLAY") == 0, sync = strcmp(word, "SYNC") == 0;
  text(d, word, 4, 13, 2);
  indicators(d, {bt, false, seek, play, sync});
  wedge(d, sync ? 0 : (vol + 2) / 5);
}
inline void drawPairing(Adafruit_GFX &d, uint32_t nowMs) {
  bool on = (nowMs / 500) % 2 == 0;
  if (on) text(d, "PAIR", 4, 13, 2); else digits(d, "    ");
  indicators(d, {on, false, false, false, false}); wedge(d, 0);
}
inline void drawStandby(Adafruit_GFX &d) { digits(d, "    "); wedge(d, 0); }

// ---------- menu ----------
// One item per screen, car-stereo style (like BASS / TREB / FADE on an old head unit).
//   index/count : "03/12" counter in the top right
//   label       : item name, size 2, max 10 chars
//   value       : current setting text ("ON", "AUTO", "5%", "S1 KNOB"), or nullptr
//   bar         : 0..20 to show the value as a wedge slider, or -1 for none
//   editing     : true while the knob is changing this value (value blinks, arrows show)
inline void drawMenu(Adafruit_GFX &d, uint8_t index, uint8_t count, const char *label,
                     const char *value, int8_t bar, bool editing, uint32_t nowMs) {
  char hdr[8]; snprintf(hdr, sizeof hdr, "%02u/%02u", (unsigned)(index + 1), (unsigned)count);
  text(d, editing ? "SET" : "MENU", 4, 1, 1);
  text(d, hdr, 128 - textW(hdr, 1) - 2, 1, 1);
  d.drawFastHLine(0, 10, 128, ON);
  if (!editing) text(d, ">", 0, 17, 2);
  text(d, label, editing ? 4 : 14, 17, 2);
  bool blinkOn = !editing || (nowMs / 300) % 2 == 0;
  if (value && blinkOn) {
    if (editing) { text(d, "<", 4, 37, 1); text(d, value, 14, 37, 1); text(d, ">", 14 + textW(value, 1) + 4, 37, 1); }
    else text(d, value, 14, 37, 1);
  }
  if (bar >= 0) wedge(d, (uint8_t)bar);
  else { d.drawFastHLine(0, 52, 128, ON); text(d, editing ? "TURN:SET PRESS:OK" : "PRESS:OPEN PLAY:BACK", 4, 55, 1); }
}

// ---------- cow sprite ----------
static const char *const COW_BODY[11] = {
  "..................#..#..", ".................######.", ".................##.###.", "#.#####################.",
  ".#################.#####", "..##..####...######.##.#", "..##..####...#######.##.", "..#####....#########....",
  "..#####....####..###....", "..#################.....", "..################......"};
static const char *const LEGS_A[3] = {"...##..##....##..##.....", "...##..##....##..##.....", "...##..##....##..##....."};
static const char *const LEGS_B[3] = {"..##...##.....##...##...", "..##...##.....##...##...", "..##...##.....##...##..."};
inline void rows(Adafruit_GFX &d, const char *const *r, uint8_t n, int16_t x, int16_t y) {
  for (uint8_t j = 0; j < n; j++) for (uint8_t i = 0; r[j][i]; i++) if (r[j][i] == '#') d.drawPixel(x + i, y + j, ON);
}
inline void cow(Adafruit_GFX &d, int16_t x, int16_t y, bool walking, uint32_t t) {
  rows(d, COW_BODY, 11, x, y);
  rows(d, (walking && (t / 150) % 2) ? LEGS_B : LEGS_A, 3, x, y + 11);
}

// ---------- boot animation (12 s design loop; plays once at power-on) ----------
static const uint32_t BOOT_MS = 10900;   // ends on the volume screen; caller switches to live UI after this
inline float easeOut(float p) { if (p < 0) p = 0; if (p > 1) p = 1; float q = 1 - p; return 1 - q * q * q; }
// Draws the boot frame for time t (ms since power-on). Returns false once the boot is finished.
inline bool drawBoot(Adafruit_GFX &d, uint32_t t, int startVol = 50) {
  if (t >= BOOT_MS) return false;
  if (t < 1200) {                                   // segment self-test: everything lit
    digits(d, "8888"); indicators(d, {true, true, true, true, true}); wedge(d, 20);
  } else if (t < 1500) {                            // power on
    drawStandby(d);
  } else if (t < 2900) {                            // volume sweep up and down
    float p = (t - 1500) / 1400.0f; int n = (int)roundf(20 * (p < 0.5f ? p * 2 : (1 - p) * 2));
    char b[5]; snprintf(b, sizeof b, "%d", n * 5);
    digits(d, b); indicators(d, {false, false, false, false, true}); wedge(d, n);
  } else if (t < 4300) {                            // tAAC + cow walks in
    digits(d, "tAAC");
    cow(d, (int16_t)(-24 + ((t - 2900) / 1400.0f) * 76), 47, true, t);
  } else if (t < 7600) {                            // scrolling name, cow stands then walks off
    digits(d, "tAAC");
    const char *msg = "THREE ACRES AND A COW PRESENTS";
    int16_t span = 128 + textW(msg, 1);
    text(d, msg, (int16_t)(128 - (t - 4300) * (span / 3300.0f)), 39, 1);
    if (t < 6600) cow(d, 52, 47, false, t);
    else cow(d, (int16_t)(52 + ((t - 6600) / 1000.0f) * 78), 47, true, t);
  } else if (t < 9400) {                            // title + loading wedge
    textC(d, "MODULAR", 8, 2);
    textC(d, "INPUT BOARD", 28, 1);
    wedge(d, t < 7900 ? 0 : (uint8_t)min<uint32_t>(20, (t - 7900) * 20 / 1400 + 1));
  } else {                                          // land on the volume screen
    drawVolume(d, startVol, true);
  }
  return true;
}

}  // namespace carui
