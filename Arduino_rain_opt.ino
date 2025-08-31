/* arduino_matrix_rain.ino (memory-optimized, PROGMEM charset, F() flash-strings)

Matrix-style "rain" VT102/ANSI screensaver over Serial for Arduino.

No per-cell brightness buffer (brightness computed on the fly).

Charset in PROGMEM to reduce SRAM.

All terminal string literals use F() to keep them in flash.

Avoids snprintf; term_move writes parts using Serial.print + F(). */


#include <Arduino.h> #include <avr/pgmspace.h>

// ---------------------- CONFIG ---------------------- const uint32_t SERIAL_BAUD = 115200; const uint8_t TERM_COLS = 60; const uint8_t TERM_ROWS = 24; const uint16_t FRAME_MS = 60; const uint8_t MAX_TRAIL = min((uint8_t)TERM_ROWS, (uint8_t)12);

// Charset stored in PROGMEM (flash) const char charset[] PROGMEM = "abcdefghijklmnopqrstuvwxyz0123456789@#$%&*()<>/\";

// ---------------------- VECTORIZED STATE ---------------------- struct Column { int8_t head; uint8_t trail; uint8_t speed; uint8_t age; uint32_t rng; };

Column cols[TERM_COLS];

// ---------------------- TINY HASH / PRNG ---------------------- static inline uint32_t splitmix32(uint32_t x) { x += 0x9e3779b9u; x = (x ^ (x >> 16)) * 0x85ebca6bu; x = (x ^ (x >> 13)) * 0xc2b2ae35u; x = x ^ (x >> 16); return x; } static inline uint32_t nextRng(uint32_t &st) { st = splitmix32(st); return st; }

static inline char pickChar(uint32_t h) { uint8_t idx = (uint8_t)(h % (sizeof(charset) - 1)); return (char)pgm_read_byte(&charset[idx]); }

// ---------------------- TERMINAL HELPERS ---------------------- static inline void term_clear() { Serial.print(F("\x1b[2J\x1b[H")); } static inline void term_hide_cursor() { Serial.print(F("\x1b[?25l")); } static inline void term_show_cursor() { Serial.print(F("\x1b[?25h")); } // move cursor to row r (1-based), col c (1-based) static inline void term_move(uint8_t r, uint8_t c) { Serial.print(F("\x1b[")); Serial.print((unsigned)r); Serial.print(F(";")); Serial.print((unsigned)c); Serial.print(F("H")); } // set color using SGR sequences (brightness 0..3) static inline void term_set_color(uint8_t bright) { switch (bright) { case 3: Serial.print(F("\x1b[97;1m")); break; // bright white case 2: Serial.print(F("\x1b[32;1m")); break; // bright green case 1: Serial.print(F("\x1b[32m")); break;   // green default: Serial.print(F("\x1b[90m")); break;  // dim gray } } static inline void term_reset_color() { Serial.print(F("\x1b[0m")); }

// ---------------------- INITIALIZATION ---------------------- void setupColumns() { for (uint8_t c = 0; c < TERM_COLS; ++c) { uint32_t seed = ((uint32_t)micros()) ^ ((uint32_t)c * 0x9e3779b1u); cols[c].rng = splitmix32(seed); cols[c].head = -1; cols[c].trail = (nextRng(cols[c].rng) % (MAX_TRAIL - 2)) + 3; cols[c].speed = (nextRng(cols[c].rng) % 5) + 1; cols[c].age = 0; } }

void maybeSpawnHeads() { for (uint8_t c = 0; c < TERM_COLS; ++c) { if (cols[c].head == -1) { uint32_t v = nextRng(cols[c].rng); if ((v & 0xFF) < 24) { cols[c].head = 0; cols[c].trail = (v % (MAX_TRAIL - 2)) + 3; cols[c].speed = (v >> 8) % 5 + 1; cols[c].age = 0; } } } }

void stepColumns() { for (uint8_t c = 0; c < TERM_COLS; ++c) { Column &col = cols[c]; if (col.head != -1) { col.age++; if (col.age >= col.speed) { col.age = 0; col.head++; if (col.head >= TERM_ROWS + col.trail) { if ((nextRng(col.rng) & 0x7F) < 48) col.head = -1; else col.head = 0; } } } } }

uint8_t brightnessFor(uint8_t c, uint8_t r) { Column &col = cols[c]; if (col.head == -1) return 0; int dist = (int)col.head - (int)r; if (dist < 0 || dist > col.trail) return 0; if (dist == 0) return 3; if (dist == 1) return 2; if (dist == 2) return 1; return 1; }

char charFor(uint8_t c, uint8_t r, uint32_t salt) { uint32_t h = (uint32_t)c * 0x9e3779b1u + (uint32_t)r * 0x85ebca6bu + salt; return pickChar(splitmix32(h)); }

void renderFrame(uint32_t salt) { Serial.print(F("\x1b[H")); for (uint8_t r = 0; r < TERM_ROWS; ++r) { for (uint8_t c = 0; c < TERM_COLS; ++c) { uint8_t b = brightnessFor(c, r); term_set_color(b); char ch = (b == 0) ? ' ' : charFor(c, r, salt + ((uint32_t)r << 8)); Serial.write(ch); term_reset_color(); } Serial.write('\n'); } }

void setup() { Serial.begin(SERIAL_BAUD); // short wait for USB serial on some boards uint32_t t0 = millis(); while (!Serial && (millis() - t0) < 1000) { } term_hide_cursor(); term_clear(); setupColumns(); }

void loop() { static uint32_t salt = 0x12345678u; maybeSpawnHeads(); stepColumns(); renderFrame(salt); salt = splitmix32(salt); delay(FRAME_MS); }

