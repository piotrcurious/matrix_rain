/*
  arduino_matrix_rain_fixed_v2.ino
  Memory-efficient Matrix rain; fixes for drops stuck at top + correct deactivation.
*/

#include <Arduino.h>

// ---------------------- CONFIG ----------------------
const uint32_t SERIAL_BAUD = 115200;
const uint8_t TERM_COLS = 80;
const uint8_t TERM_ROWS = 24;

const uint16_t FRAME_MS = 60;
const uint8_t MAX_DROPS = 25;
const uint8_t MAX_TRAIL = 12;
const uint8_t SPAWN_CHANCE = 35; // out of 256
const char CHARSET[] = "abcdefghijklmnopqrstuvwxyz0123456789@#$%&*()<>/\"";

// ---------------------- STATE ----------------------
struct Drop {
  uint8_t col;      // Column (x-position)
  int16_t head;     // Head row; -1 = inactive
  uint8_t trail;    // Trail length
  uint8_t speed;    // Frames per step
  uint8_t age;      // Frame counter
  uint32_t rng;     // Per-drop RNG state
};

Drop drops[MAX_DROPS];
bool column_occupied[TERM_COLS];
uint32_t global_rng = 0xA5A5A5A5u; // seed for spawning

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
  return CHARSET[h % (sizeof(CHARSET) - 1)];
}

// ---------------------- TERMINAL HELPERS ----------------------
static inline void term_clear() { Serial.write("\x1b[2J"); Serial.write("\x1b[H"); }
static inline void term_hide_cursor() { Serial.write("\x1b[?25l"); }
static inline void term_show_cursor() { Serial.write("\x1b[?25h"); }

static inline void term_set_color(uint8_t bright) {
  switch (bright) {
    case 3: Serial.write("\x1b[97;1m"); break; // Bright White (head)
    case 2: Serial.write("\x1b[32;1m"); break; // Bright Green
    case 1: Serial.write("\x1b[32m");   break; // Normal Green
    default: Serial.write("\x1b[90m");  break; // Dark Gray / empty
  }
}
static inline void term_reset_color() { Serial.write("\x1b[0m"); }

// ---------------------- UTIL: compute rolling char for a drop at distance dist
char charForDrop(const Drop &d, int16_t dist) {
  // Mix drop RNG with distance so characters "roll" as head moves.
  uint32_t h = d.rng + (uint32_t)dist * 0x9e3779b1u;
  h = splitmix32(h);
  return pickChar(h);
}

// ---------------------- INITIALIZATION ----------------------
void setupState() {
  for (uint8_t i = 0; i < MAX_DROPS; ++i) drops[i].head = -1;
  for (uint8_t c = 0; c < TERM_COLS; ++c) column_occupied[c] = false;
}

// ---------------------- SPAWN ----------------------
void maybeSpawnDrop() {
  // advance global RNG and check chance
  global_rng = splitmix32(global_rng);
  if ((global_rng & 0xFF) > SPAWN_CHANCE) return;

  // find an inactive slot
  int16_t idx = -1;
  for (uint8_t i = 0; i < MAX_DROPS; ++i) {
    if (drops[i].head == -1) { idx = i; break; }
  }
  if (idx == -1) return;

  // pick a free column (randomized start)
  global_rng = splitmix32(global_rng);
  uint8_t start_col = global_rng % TERM_COLS;
  int16_t free_col = -1;
  for (uint8_t i = 0; i < TERM_COLS; ++i) {
    uint8_t c = (start_col + i) % TERM_COLS;
    if (!column_occupied[c]) { free_col = c; break; }
  }
  if (free_col == -1) return;

  // initialize the drop
  Drop &d = drops[idx];
  global_rng = splitmix32(global_rng);

  d.col   = (uint8_t)free_col;
  d.head  = 0; // spawn at very top row
  d.trail = (global_rng % (MAX_TRAIL - 2)) + 3; // 3..MAX_TRAIL-? 
  d.speed = ((global_rng >> 8) % 5) + 1;       // 1..5 frames per step
  d.age   = 0;
  d.rng   = splitmix32(global_rng);            // seed per-drop RNG

  column_occupied[d.col] = true;
}

// ---------------------- STEP ----------------------
void stepDrops() {
  for (uint8_t i = 0; i < MAX_DROPS; ++i) {
    Drop &d = drops[i];
    if (d.head == -1) continue; // inactive

    d.age++;
    if (d.age >= d.speed) {
      d.age = 0;
      d.head++;                  // move head down one row
      d.rng = splitmix32(d.rng); // advance per-drop RNG so stream changes as it moves
    }

    // Deactivate when the bottom-most visible part of the trail has passed the last row:
    // bottom_of_trail_row = d.head - (d.trail - 1)
    // if bottom_of_trail_row >= TERM_ROWS -> everything off-screen
    if (d.head - (int16_t)(d.trail - 1) >= (int16_t)TERM_ROWS) {
      column_occupied[d.col] = false;
      d.head = -1;
    }
  }
}

// ---------------------- RENDER ----------------------
void renderFrame(uint32_t salt) {
  // home cursor to overwrite screen in-place (no scrolling)
  Serial.write("\x1b[H");
  uint8_t last_brightness = 255;

  for (uint8_t r = 0; r < TERM_ROWS; ++r) {
    for (uint8_t c = 0; c < TERM_COLS; ++c) {
      uint8_t current_brightness = 0;
      char ch = ' ';

      if (column_occupied[c]) {
        // find the (single) drop in this column
        for (uint8_t i = 0; i < MAX_DROPS; ++i) {
          const Drop &d = drops[i];
          if (d.head == -1 || d.col != c) continue;

          int16_t dist = d.head - (int16_t)r; // 0 = head at this row; positive = head below row
          if (dist >= 0 && dist < d.trail) {
            // brightness mapping: head=3, next=2, next=1, rest=1 (dim)
            if (dist == 0) current_brightness = 3;
            else if (dist < 4) current_brightness = 3 - dist;
            else current_brightness = 1;

            ch = charForDrop(d, dist);
            break; // only one drop per column can apply
          }
        }
      }

      if (current_brightness != last_brightness) {
        term_set_color(current_brightness);
        last_brightness = current_brightness;
      }
      Serial.write(ch);
    }
    if (r < TERM_ROWS - 1) {
       // move to next row, column 1 (rows/cols are 1-indexed for ANSI)
        char buf[16];
        snprintf(buf, sizeof(buf), "\x1b[%u;%uH", (unsigned)(r + 2), 1u);
        Serial.write(buf);
     }

  }
  term_reset_color();
}

// ---------------------- MAIN ----------------------
void setup() {
  Serial.begin(SERIAL_BAUD);
  uint32_t start = millis();
  while (!Serial && (millis() - start < 1000)) { delay(1); }

  delay(50);
  term_hide_cursor();
  term_clear();
  setupState();
}

void loop() {
  static uint32_t salt = 0x12345678u;

  maybeSpawnDrop();
  stepDrops();
  renderFrame(salt);

  salt = splitmix32(salt);
  delay(FRAME_MS);
}

void cleanupAndShowCursor() {
  term_show_cursor();
  term_reset_color();
}
