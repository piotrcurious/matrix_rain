/* arduino_matrix_rain.ino (memory-optimized, PROGMEM charset, F() flash-strings, RTC-driven palettes)
*/

#include <Arduino.h>
#include <avr/pgmspace.h>

// ---------------------- CONFIG ----------------------
const uint32_t SERIAL_BAUD = 115200;
const uint8_t TERM_COLS = 60;
const uint8_t TERM_ROWS = 24;
const uint16_t FRAME_MS = 60;
const uint8_t MAX_TRAIL = min((uint8_t)TERM_ROWS, (uint8_t)12);

// Charset in flash
const char charset[] PROGMEM = "abcdefghijklmnopqrstuvwxyz0123456789@#$%&*()<>/";

// ---------------------- PALETTES (PROGMEM) ----------------------
const uint8_t palettes[][4] PROGMEM = {
  {235, 24, 37, 51}, {233, 17, 18, 20}, {237, 131, 166, 214}, {236, 64, 70, 120},
  {250, 118, 46, 82}, {238, 166, 172, 220}, {237, 125, 129, 201}, {232, 234, 240, 249}
};
const uint8_t PALETTE_COUNT = sizeof(palettes) / sizeof(palettes[0]);

// ---------------------- STATE ----------------------
struct Column {
  int8_t head;
  uint8_t trail;
  uint8_t speed;
  uint8_t age;
  uint32_t rng;
};

Column cols[TERM_COLS];

// Forward declarations
uint32_t splitmix32(uint32_t x);
uint32_t nextRng(uint32_t &st);
char pickChar(uint32_t h);
void term_clear();
void term_hide_cursor();
void term_show_cursor();
void term_emit_256color(uint8_t colorIndex);
void term_set_color_from_palette(uint8_t paletteIndex, uint8_t level, bool bold);
void term_reset_color();
void setupColumns();
void maybeSpawnHeads();
void stepColumns();
uint8_t brightnessFor(uint8_t c, uint8_t r);
char charFor(uint8_t c, uint8_t r, uint32_t salt);
uint8_t readHour();
uint8_t paletteIndexForHour(uint8_t hour);
void updatePaletteFromRTC();
void renderFrame(uint32_t salt);

// ---------------------- TINY HASH / PRNG ----------------------
static inline uint32_t splitmix32(uint32_t x) {
  x += 0x9e3779b9u;
  x = (x ^ (x >> 16)) * 0x85ebca6bu;
  x = (x ^ (x >> 13)) * 0xc2b2ae35u;
  x = x ^ (x >> 16);
  return x;
}
static inline uint32_t nextRng(uint32_t &st) {
  st = splitmix32(st);
  return st;
}
static inline char pickChar(uint32_t h) {
  uint8_t idx = (uint8_t)(h % (sizeof(charset) - 1));
  return (char)pgm_read_byte(&charset[idx]);
}

// ---------------------- TERMINAL HELPERS ----------------------
static inline void term_clear() { Serial.print(F("\x1b[2J\x1b[H")); }
static inline void term_hide_cursor() { Serial.print(F("\x1b[?25l")); }
static inline void term_show_cursor() { Serial.print(F("\x1b[?25h")); }
static inline void term_emit_256color(uint8_t colorIndex) {
  Serial.print(F("\x1b[38;5;"));
  Serial.print((unsigned)colorIndex);
  Serial.print(F("m"));
}
static inline void term_set_color_from_palette(uint8_t paletteIndex, uint8_t level, bool bold) {
  if (paletteIndex >= PALETTE_COUNT) paletteIndex = 0;
  uint8_t color = pgm_read_byte(&palettes[paletteIndex][level]);
  term_emit_256color(color);
  if (bold) Serial.print(F("\x1b[1m"));
}
static inline void term_reset_color() { Serial.print(F("\x1b[0m")); }

// ---------------------- INITIALIZATION ----------------------
void setupColumns() {
  for (uint8_t c = 0; c < TERM_COLS; ++c) {
    uint32_t seed = ((uint32_t)micros()) ^ ((uint32_t)c * 0x9e3779b1u);
    cols[c].rng = splitmix32(seed);
    cols[c].head = -1;
    cols[c].trail = (nextRng(cols[c].rng) % (MAX_TRAIL - 2)) + 3;
    cols[c].speed = (nextRng(cols[c].rng) % 5) + 1;
    cols[c].age = 0;
  }
}

void maybeSpawnHeads() {
  for (uint8_t c = 0; c < TERM_COLS; ++c) {
    if (cols[c].head == -1) {
      uint32_t v = nextRng(cols[c].rng);
      if ((v & 0xFF) < 24) {
        cols[c].head = 0;
        cols[c].trail = (v % (MAX_TRAIL - 2)) + 3;
        cols[c].speed = (v >> 8) % 5 + 1;
        cols[c].age = 0;
      }
    }
  }
}

void stepColumns() {
  for (uint8_t c = 0; c < TERM_COLS; ++c) {
    Column &col = cols[c];
    if (col.head != -1) {
      col.age++;
      if (col.age >= col.speed) {
        col.age = 0;
        col.head++;
        if (col.head >= TERM_ROWS + col.trail) {
          if ((nextRng(col.rng) & 0x7F) < 48) col.head = -1;
          else col.head = 0;
        }
      }
    }
  }
}

uint8_t brightnessFor(uint8_t c, uint8_t r) {
  Column &col = cols[c];
  if (col.head == -1) return 0;
  int dist = (int)col.head - (int)r;
  if (dist < 0 || dist > col.trail) return 0;
  if (dist == 0) return 3;
  if (dist == 1) return 2;
  return 1;
}

char charFor(uint8_t c, uint8_t r, uint32_t salt) {
  uint32_t h = (uint32_t)c * 0x9e3779b1u + (uint32_t)r * 0x85ebca6bu + salt;
  return pickChar(splitmix32(h));
}

// ---------------------- RTC fallback ----------------------
uint32_t simulated_start_ms = 0;
uint8_t readHour() {
  uint32_t elapsed = (millis() - simulated_start_ms) / 1000;
  return (12 + (elapsed / 3600)) % 24;
}
uint8_t paletteIndexForHour(uint8_t hour) {
  if (hour < 4) return 0;
  if (hour < 6) return 1;
  if (hour < 8) return 2;
  if (hour < 12) return 3;
  if (hour < 16) return 4;
  if (hour < 18) return 5;
  if (hour < 20) return 6;
  return 0;
}

uint8_t currentPaletteIndex = 0;
void updatePaletteFromRTC() {
  currentPaletteIndex = paletteIndexForHour(readHour());
}

void renderFrame(uint32_t salt) {
  updatePaletteFromRTC();
  Serial.print(F("\x1b[H"));
  for (uint8_t r = 0; r < TERM_ROWS; ++r) {
    for (uint8_t c = 0; c < TERM_COLS; ++c) {
      uint8_t b = brightnessFor(c, r);
      if (b == 0) {
        term_set_color_from_palette(currentPaletteIndex, 0, false);
        Serial.write(' ');
        term_reset_color();
        continue;
      }
      bool bold = (b == 3);
      uint8_t level = (b >= 3) ? 3 : ((b == 2) ? 2 : 1);
      term_set_color_from_palette(currentPaletteIndex, level, bold);
      char ch = charFor(c, r, salt + ((uint32_t)r << 8));
      Serial.write(ch);
      term_reset_color();
    }
    Serial.write('\n');
  }
}

void setup() {
  Serial.begin(SERIAL_BAUD);
  uint32_t t0 = millis();
  while (!Serial && (millis() - t0) < 1000) { }
  simulated_start_ms = millis();
  term_hide_cursor();
  term_clear();
  setupColumns();
}

void loop() {
  static uint32_t salt = 0x12345678u;
  maybeSpawnHeads();
  stepColumns();
  renderFrame(salt);
  salt = splitmix32(salt);
  delay(FRAME_MS);
}
