/* arduino_matrix_rain.ino (memory-optimized, PROGMEM charset, F() flash-strings, RTC-driven palettes)

Features added in this revision:

Uses a real-time clock (DS3231 via RTClib) to determine local time of day.

Selects one of several carefully crafted 256-color palettes based on hour-of-day to simulate changing daylight and nocturnal neon streetlight glows.

Palettes are stored in PROGMEM as 8-bit 256-color indices (xterm-256 color).

term_set_color() now emits 256-color SGR sequences (ESC[38;5;<N>m) using palette colors.

If no RTC is available, the sketch falls back to a simulated clock driven by millis().


Notes:

This sketch uses Adafruit/RTClib style DateTime/RTC_DS3231. Install RTClib if you want native RTC support.

The terminal must support ANSI 256-color (modern xterm, iTerm2, most linux terminals).


Copyright: public domain / do whatever you want. */

#include <Arduino.h> #include <Wire.h> #include <avr/pgmspace.h>

// Optional RTC library. If you don't have RTClib installed, the code will still compile // but with a simulated clock fallback. To enable full RTC support, install RTClib and // uncomment the include and RTC object below. #ifdef USE_RTCLIB #include <RTClib.h> RTC_DS3231 rtc; #endif

// ---------------------- CONFIG ---------------------- const uint32_t SERIAL_BAUD = 115200; const uint8_t TERM_COLS = 60; const uint8_t TERM_ROWS = 24; const uint16_t FRAME_MS = 60; const uint8_t MAX_TRAIL = min((uint8_t)TERM_ROWS, (uint8_t)12);

// Charset in flash const char charset[] PROGMEM = "abcdefghijklmnopqrstuvwxyz0123456789@#$%&*()<>/";

// ---------------------- PALETTES (PROGMEM) ---------------------- // Each palette contains 4 colors (indices into xterm-256 palette) for brightness levels // [dim, low, mid, head]. Carefully chosen to create a rich progression through the day. const uint8_t palettes[][4] PROGMEM = { // 0 - Night neon street (cold cyan neon + dark base) {235, 24, 37, 51}, // 1 - Predawn blue (very deep blues) {233, 17, 18, 20}, // 2 - Sunrise warm (pink -> orange -> yellow head) {237, 131, 166, 214}, // 3 - Morning soft green (soft, fresh greens) {236, 64, 70, 120}, // 4 - Midday bright (vibrant greens) {250, 118, 46, 82}, // 5 - Afternoon gold (warmer, golden tones) {238, 166, 172, 220}, // 6 - Dusk magenta (rich purples/magenta) {237, 125, 129, 201}, // 7 - Night deep (muted grays for very late quiet hours) {232, 234, 240, 249} }; const uint8_t PALETTE_COUNT = sizeof(palettes) / sizeof(palettes[0]);

// ---------------------- VECTORIZED STATE ---------------------- struct Column { int8_t head; uint8_t trail; uint8_t speed; uint8_t age; uint32_t rng; };

Column cols[TERM_COLS];

// ---------------------- TINY HASH / PRNG ---------------------- static inline uint32_t splitmix32(uint32_t x) { x += 0x9e3779b9u; x = (x ^ (x >> 16)) * 0x85ebca6bu; x = (x ^ (x >> 13)) * 0xc2b2ae35u; x = x ^ (x >> 16); return x; } static inline uint32_t nextRng(uint32_t &st) { st = splitmix32(st); return st; } static inline char pickChar(uint32_t h) { uint8_t idx = (uint8_t)(h % (sizeof(charset) - 1)); return (char)pgm_read_byte(&charset[idx]); }

// ---------------------- TERMINAL HELPERS ---------------------- static inline void term_clear() { Serial.print(F("[2J[H")); } static inline void term_hide_cursor() { Serial.print(F("[?25l")); } static inline void term_show_cursor() { Serial.print(F("[?25h")); }

// Emit 256-color SGR sequence for a given color index (0..255). static inline void term_emit_256color(uint8_t colorIndex) { Serial.print(F("[38;5;")); Serial.print((unsigned)colorIndex); Serial.print(F("m")); } static inline void term_set_color_from_palette(uint8_t paletteIndex, uint8_t level, bool bold) { // level: 0..3 -> dim..head if (paletteIndex >= PALETTE_COUNT) paletteIndex = 0; uint8_t color = pgm_read_byte_near(&palettes[paletteIndex][level]); term_emit_256color(color); if (bold) Serial.print(F("[1m")); } static inline void term_reset_color() { Serial.print(F("[0m")); }

// ---------------------- INITIALIZATION ---------------------- void setupColumns() { for (uint8_t c = 0; c < TERM_COLS; ++c) { uint32_t seed = ((uint32_t)micros()) ^ ((uint32_t)c * 0x9e3779b1u); cols[c].rng = splitmix32(seed); cols[c].head = -1; cols[c].trail = (nextRng(cols[c].rng) % (MAX_TRAIL - 2)) + 3; cols[c].speed = (nextRng(cols[c].rng) % 5) + 1; cols[c].age = 0; } }

void maybeSpawnHeads() { for (uint8_t c = 0; c < TERM_COLS; ++c) { if (cols[c].head == -1) { uint32_t v = nextRng(cols[c].rng); if ((v & 0xFF) < 24) { cols[c].head = 0; cols[c].trail = (v % (MAX_TRAIL - 2)) + 3; cols[c].speed = (v >> 8) % 5 + 1; cols[c].age = 0; } } } }

void stepColumns() { for (uint8_t c = 0; c < TERM_COLS; ++c) { Column &col = cols[c]; if (col.head != -1) { col.age++; if (col.age >= col.speed) { col.age = 0; col.head++; if (col.head >= TERM_ROWS + col.trail) { if ((nextRng(col.rng) & 0x7F) < 48) col.head = -1; else col.head = 0; } } } } }

// Compute brightness (0..3) based on distance to head. Simple, cheap function. uint8_t brightnessFor(uint8_t c, uint8_t r) { Column &col = cols[c]; if (col.head == -1) return 0; int dist = (int)col.head - (int)r; if (dist < 0 || dist > col.trail) return 0; if (dist == 0) return 3; if (dist == 1) return 2; if (dist == 2) return 1; return 1; }

char charFor(uint8_t c, uint8_t r, uint32_t salt) { uint32_t h = (uint32_t)c * 0x9e3779b1u + (uint32_t)r * 0x85ebca6bu + salt; return pickChar(splitmix32(h)); }

// ---------------------- RTC & PALETTE SELECTION ---------------------- uint8_t currentPaletteIndex = 0;

// Fallback simulated clock if RTC not available uint32_t simulated_start_ms = 0; // set at setup() uint8_t simulated_start_hour = 12; // choose midday start by default

// Read current hour (0..23). Tries RTC first (if compiled with USE_RTCLIB), otherwise uses simulated clock. uint8_t readHour() { #ifdef USE_RTCLIB if (rtc.begin()) { DateTime now = rtc.now(); return now.hour(); } #endif // fallback: simulate a day that loops every 246060*1000 ms uint32_t elapsed = (millis() - simulated_start_ms) / 1000; // seconds elapsed uint32_t daysec = elapsed % 86400UL; uint8_t hour = (simulated_start_hour + (daysec / 3600)) % 24; return hour; }

// Map hour -> palette index. Ranges chosen to create a pleasing variety across the day. uint8_t paletteIndexForHour(uint8_t hour) { if (hour < 4) return 0;       // deep night / street neon (00:00-03:59) if (hour < 6) return 1;       // predawn (04:00-05:59) if (hour < 8) return 2;       // sunrise (06:00-07:59) if (hour < 12) return 3;      // morning (08:00-11:59) if (hour < 16) return 4;      // midday (12:00-15:59) if (hour < 18) return 5;      // afternoon (16:00-17:59) if (hour < 20) return 6;      // dusk (18:00-19:59) return 0;                     // evening/night (20:00-23:59) -> neon/street again }

void updatePaletteFromRTC() { uint8_t hour = readHour(); uint8_t idx = paletteIndexForHour(hour); currentPaletteIndex = idx; }

// ---------------------- RENDER ---------------------- void renderFrame(uint32_t salt) { // update palette once per frame (cheap) updatePaletteFromRTC();

Serial.print(F("[H")); for (uint8_t r = 0; r < TERM_ROWS; ++r) { for (uint8_t c = 0; c < TERM_COLS; ++c) { uint8_t b = brightnessFor(c, r); if (b == 0) { // draw space with very dim color to keep contrast at night themes term_set_color_from_palette(currentPaletteIndex, 0, false); Serial.write(' '); term_reset_color(); continue; } bool bold = (b == 3); // head is bold // For night street neon we want the head extra-bright: if palette==0 and head, print an extra bold if (currentPaletteIndex == 0 && b == 3) bold = true; // pick level mapping: b==3 -> level 3, b==2 -> 2, else -> 1 uint8_t level = (b >= 3) ? 3 : ((b == 2) ? 2 : 1); term_set_color_from_palette(currentPaletteIndex, level, bold); char ch = charFor(c, r, salt + ((uint32_t)r << 8)); Serial.write(ch); term_reset_color(); } Serial.write(' '); } }

// ---------------------- MAIN ---------------------- void setup() { Serial.begin(SERIAL_BAUD); // short wait for USB serial on some boards uint32_t t0 = millis(); while (!Serial && (millis() - t0) < 1000) { }

#ifdef USE_RTCLIB // attempt to init RTC if (!rtc.begin()) { // RTC not found - fall back to simulated clock simulated_start_ms = millis(); } #else simulated_start_ms = millis(); #endif

term_hide_cursor(); term_clear(); setupColumns(); }

void loop() { static uint32_t salt = 0x12345678u; maybeSpawnHeads(); stepColumns(); renderFrame(salt); salt = splitmix32(salt); delay(FRAME_MS); } /* arduino_matrix_rain.ino (memory-optimized, PROGMEM charset, F() flash-strings)

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

