/* arduino_matrix_rain.ino

Matrix-style "rain" VT102/ANSI screensaver over Serial for Arduino.

Uses compact vectorized state per-column

Uses a tiny hashing/PRNG to produce reproducible streams per-column

Writes VT102 escape sequences to produce color + cursor movement

Works best with an external terminal that understands ANSI (PuTTY, screen, minicom, iTerm2)


Usage:

Upload to your Arduino (any AVR/ARM board should work)

Open a VT102/ANSI-capable terminal at 115200 8N1 (not the Arduino IDE Monitor)


Notes on choices:

Columns are processed in a mostly vectorized manner: each column has a bitmask-like "trail" stored as bytes (brightness 0..3) and we shift them each tick. That lets us update entire columns with compact ops.

We use a very small and fast splitmix-like hash to create deterministic pseudo-random sequences per column (so columns differ visually but are deterministic for debugging).

The output is clipped to terminal size constants; change TERM_COLS and TERM_ROWS to match your terminal if you want a specific behavior.


Copyright: public domain / do whatever you want. */

#include <Arduino.h>

// ---------------------- CONFIG ---------------------- const uint32_t SERIAL_BAUD = 115200; // Terminal size; adjust to match your terminal. Avoid huge sizes on AVR due to memory. const uint8_t TERM_COLS = 60; // number of vertical rain columns (characters per row) const uint8_t TERM_ROWS = 24; // number of rows (height)

const uint16_t FRAME_MS = 60; // base frame time in milliseconds

// Visual tuning const uint8_t MAX_TRAIL = min((uint8_t)TERM_ROWS, (uint8_t)12); // maximum trail length const char charset[] = "abcdefghijklmnopqrstuvwxyz0123456789@#$%&*()<>/\"; // characters used

// ---------------------- VECTORIZED STATE ---------------------- // Each column has: head position (0..TERM_ROWS-1 or -1 invisible), current trail length (1..MAX_TRAIL), // speed (1..4) measured in frames per movement, and a tiny PRNG state struct Column { int8_t head;        // head row index, -1 means inactive uint8_t trail;      // current trail length (1..MAX_TRAIL) uint8_t speed;      // frames per step (1..6) uint8_t age;        // age counter to step timing uint32_t rng;       // per-column PRNG state // brightness map for the column: 2 bits per row (0..3), packed low-to-high in bytes // We'll allocate TERM_ROWS bytes per column (simple and clear) — memory ok for moderate sizes };

Column cols[TERM_COLS]; uint8_t brightness[TERM_COLS][TERM_ROWS]; // small matrix holding 0..3 brightness per cell

// ---------------------- TINY HASH / PRNG ---------------------- // splitmix32-ish for compact 32-bit PRN — fast, tiny state static inline uint32_t splitmix32(uint32_t x) { x += 0x9e3779b9u; x = (x ^ (x >> 16)) * 0x85ebca6bu; x = (x ^ (x >> 13)) * 0xc2b2ae35u; x = x ^ (x >> 16); return x; }

// get next random from per-column state static inline uint32_t nextRng(uint32_t &st) { st = splitmix32(st); return st; }

// choose a printable char based on hashed x static inline char pickChar(uint32_t h) { uint32_t idx = h % (sizeof(charset) - 1); return charset[idx]; }

// ---------------------- TERMINAL HELPERS ---------------------- static inline void term_clear() { Serial.write("\x1b[2J"); // clear screen Serial.write("\x1b[H");  // move cursor home } static inline void term_hide_cursor() { Serial.write("\x1b[?25l"); } static inline void term_show_cursor() { Serial.write("\x1b[?25h"); } // move cursor to row r (1-based), col c (1-based) static inline void term_move(uint8_t r, uint8_t c) { char buf[16]; // format: ESC [ r ; c H snprintf(buf, sizeof(buf), "\x1b[%u;%uH", (unsigned)r, (unsigned)c); Serial.write(buf); } // set color: brightness 0..3 static inline void term_set_color(uint8_t bright) { // We'll map brightness to color codes: head=bright white, strong green, dim green, very dim gray // Use SGR sequences switch (bright) { case 3: Serial.write("\x1b[97;1m"); break; // bright white bold case 2: Serial.write("\x1b[32;1m"); break; // bright green case 1: Serial.write("\x1b[32m"); break;   // green default: Serial.write("\x1b[90m"); break;  // dark gray } }

// reset SGR static inline void term_reset_color() { Serial.write("\x1b[0m"); }

// ---------------------- INITIALIZATION ---------------------- void setupColumns() { for (uint8_t c = 0; c < TERM_COLS; ++c) { // init RNG from column index and micros() seed-ish uint32_t seed = ((uint32_t)micros()) ^ ((uint32_t)c * 0x9e3779b1u); cols[c].rng = splitmix32(seed); cols[c].head = -1; // start inactive cols[c].trail = (nextRng(cols[c].rng) % (MAX_TRAIL - 2)) + 3; // 3..MAX_TRAIL cols[c].speed = (nextRng(cols[c].rng) % 5) + 1; // 1..5 frames per step cols[c].age = (uint8_t)(nextRng(cols[c].rng) % cols[c].speed); // zero brightness for (uint8_t r = 0; r < TERM_ROWS; ++r) brightness[c][r] = 0; } }

// randomly spawn heads occasionally void maybeSpawnHeads() { for (uint8_t c = 0; c < TERM_COLS; ++c) { if (cols[c].head == -1) { // spawn with low probability influenced by RNG uint32_t v = nextRng(cols[c].rng); // introduce a small bias to avoid uniform spawns if ((v & 0xFF) < 24) { // ~9% chance per tick cols[c].head = 0; // top row cols[c].trail = (v % (MAX_TRAIL - 2)) + 3; cols[c].speed = (v >> 8) % 5 + 1; // 1..5 cols[c].age = 0; } } } }

// ---------------------- FRAME UPDATE (vectorized-ish) ---------------------- void stepColumns() { // Advance each column according to its speed, shift brightness downward for (uint8_t c = 0; c < TERM_COLS; ++c) { Column &col = cols[c]; // decay brightness: shift each row one step toward 0 for (int r = TERM_ROWS - 1; r >= 0; --r) { // fade by one per frame if (brightness[c][r] > 0) brightness[c][r]--; }

if (col.head != -1) {
  // step timing
  col.age++;
  if (col.age >= col.speed) {
    col.age = 0;
    // advance head downwards
    if (col.head < TERM_ROWS) {
      // put a bright head at col.head
      uint8_t hpos = (uint8_t)col.head;
      brightness[c][hpos] = 3; // brightest

      // push the trail below head according to trail length
      for (uint8_t t = 1; t <= col.trail; ++t) {
        int8_t pos = (int8_t)hpos - (int8_t)t;
        if (pos >= 0 && pos < TERM_ROWS) {
          // set descending brightness
          uint8_t b = 3 - (t > 3 ? 3 : t);
          // only overwrite if stronger than existing
          if (b > brightness[c][pos]) brightness[c][pos] = b;
        }
      }
      col.head++;
    } else {
      // head fell past bottom -> deactivate with some probability
      uint32_t v = nextRng(col.rng);
      if ((v & 0x7F) < 48) {
        col.head = -1; // go inactive
      } else {
        // respawn near top
        col.head = 0;
        col.trail = (v % (MAX_TRAIL - 2)) + 3;
        col.speed = (v >> 8) % 5 + 1;
      }
    }
  }
}

} }

// produce a character for a cell using hashing of column,row and a changing salt char charFor(uint8_t c, uint8_t r, uint32_t salt) { // combine column, row, salt into small hash uint32_t h = (uint32_t)c * 0x9e3779b1u + (uint32_t)r * 0x85ebca6bu + salt; h = splitmix32(h); return pickChar(h); }

// ---------------------- RENDER ---------------------- // We redraw the whole grid each frame — simpler and produces consistent look. void renderFrame(uint32_t salt) { // move cursor home Serial.write("\x1b[H");

for (uint8_t r = 0; r < TERM_ROWS; ++r) { for (uint8_t c = 0; c < TERM_COLS; ++c) { uint8_t b = brightness[c][r]; term_set_color(b); char ch = (b == 0) ? ' ' : charFor(c, r, salt + ((uint32_t)r << 8)); Serial.write(ch); term_reset_color(); } // newline Serial.write('\n'); } }

// ---------------------- MAIN ---------------------- void setup() { Serial.begin(SERIAL_BAUD); // Wait for serial to get ready on boards that support it. uint32_t start = millis(); while (!Serial && (millis() - start < 1000)) { delay(1); }

delay(50); term_hide_cursor(); term_clear(); setupColumns(); }

void loop() { static uint32_t salt = 0x12345678u; maybeSpawnHeads(); stepColumns(); renderFrame(salt); salt = splitmix32(salt); delay(FRAME_MS); }

// optional: ensure cursor is shown if board resets via watchdog / serial disconnect // (Not called automatically; user can add watchdog or button handler.)

