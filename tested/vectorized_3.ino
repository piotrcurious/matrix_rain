/*
  matrix_rain_vectorized_fixed.ino

  Vectorized, memory-conscious Matrix rain with persistent fade and
  correct fast-drop interaction & head boosting behavior.
*/

#include <Arduino.h>
#include <string.h>

// ---------------------- CONFIG ----------------------
const uint32_t SERIAL_BAUD = 115200;

// Terminal size - change to your terminal (e.g., 80x24)
const uint8_t TERM_COLS = 80;
const uint8_t TERM_ROWS = 24;

// Visual / performance knobs
const uint16_t FRAME_MS = 1;
const uint8_t MAX_DROPS = 35;
const uint8_t MAX_TRAIL = 12;      // keep small if memory constrained
const uint8_t MAX_FAST = 16;       // fast bullets
const uint8_t SPAWN_CHANCE = 45;   // (0..255) chance to spawn a crawling drop each frame
const uint8_t SPAWN_FAST_CHANCE = 28; // chance to spawn a fast bullet each frame

const char CHARSET[] = "abcdefghijklmnopqrstuvwxyz0123456789@#$%&*()<>/\"";

// ---------------------- DERIVED ----------------------
#define BRIGHT_BYTES_PER_DROP ((MAX_TRAIL * 2 + 7) / 8) // 2 bits per slot

// ---------------------- STATE ----------------------
struct Drop {
  uint8_t col;      // Column (x-position)
  int16_t head;     // Head row; -1 = inactive
  uint8_t trail;    // Trail length
  uint8_t speed;    // Frames per step
  uint8_t age;      // Frame counter
  uint32_t rng;     // Per-drop RNG
  uint8_t boost;    // boost frames remaining (frame-count)
};

struct FastDrop {
  bool active;
  uint8_t col;
  int16_t row;   // current row
  uint8_t speed; // frames per step (fast=1)
  uint8_t age;
  uint32_t rng;
};

Drop drops[MAX_DROPS];
FastDrop fasts[MAX_FAST];
bool column_occupied[TERM_COLS];
uint32_t global_rng = 0xA5A5A5A5u;

// Compact per-drop buffers (vectorized)
static uint8_t drop_char_idx[MAX_DROPS][MAX_TRAIL];                 // 1 byte per slot: index into CHARSET
static uint8_t drop_bright_packed[MAX_DROPS][BRIGHT_BYTES_PER_DROP + 1]; // +1 to safely read spanning bits

// ---------------------- PRNG / helpers ----------------------
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

static inline uint8_t pickIndex(uint32_t h) {
  return (uint8_t)(h % (sizeof(CHARSET) - 1));
}

// ---------------------- TERMINAL HELPERS ----------------------
static inline void term_clear() { Serial.write("\x1b[2J"); Serial.write("\x1b[H"); }
static inline void term_hide_cursor() { Serial.write("\x1b[?25l"); }
static inline void term_show_cursor() { Serial.write("\x1b[?25h"); }

static inline void term_set_color(uint8_t bright) {
  switch (bright) {
    case 4: Serial.write("\x1b[97;1m"); break; // super-bright white
    case 3: Serial.write("\x1b[97m"); break; // bright white
    case 2: Serial.write("\x1b[32;1m"); break; // bright green
    case 1: Serial.write("\x1b[32m"); break; // normal green
    default: Serial.write("\x1b[90m"); break; // dim gray / off
  }
}

static inline void term_reset_color() { Serial.write("\x1b[0m"); }

// ---------------------- packed brightness helpers (2 bits per slot) ----------------------

// Corrected version of get_bright_slot
static inline uint8_t get_bright_slot(uint8_t drop_idx, uint8_t slot) {
    uint16_t bitpos = (uint16_t)slot * 2;
    uint8_t byteIdx = bitpos / 8;
    uint8_t bitOff = bitpos % 8;
    uint16_t val;
    val = drop_bright_packed[drop_idx][byteIdx] | ((uint16_t)drop_bright_packed[drop_idx][byteIdx + 1] << 8);
    return (uint8_t)((val >> bitOff) & 0x03u);
}
static inline void set_bright_slot(uint8_t drop_idx, uint8_t slot, uint8_t val) {
    val &= 0x03u; // Ensure val is only 2 bits
    uint16_t bitpos = (uint16_t)slot * 2;
    uint8_t byteIdx = bitpos / 8;
    uint8_t bitOff = bitpos % 8;
    uint16_t mask = (uint16_t)~(0x03u << bitOff);
    uint16_t data = drop_bright_packed[drop_idx][byteIdx] | ((uint16_t)drop_bright_packed[drop_idx][byteIdx + 1] << 8);
    data = (data & mask) | ((uint16_t)val << bitOff);
    drop_bright_packed[drop_idx][byteIdx] = (uint8_t)data;
    drop_bright_packed[drop_idx][byteIdx + 1] = (uint8_t)(data >> 8);
}

// ---------------------- utilities ----------------------
int findDropIndexInColumn(uint8_t col) {
  for (uint8_t i = 0; i < MAX_DROPS; ++i) {
    if (drops[i].head != -1 && drops[i].col == col) return (int)i;
  }
  return -1;
}

static inline char charset_at_idx(uint8_t idx) {
  return CHARSET[idx % (sizeof(CHARSET) - 1)];
}

static inline char charForFast(const FastDrop &f) {
  // f.rng is advanced in stepFastDrops; use it directly for display
  return charset_at_idx((uint8_t)(f.rng % (sizeof(CHARSET) - 1)));
}

// ---------------------- INIT ----------------------
void setupState() {
  for (uint8_t i = 0; i < MAX_DROPS; ++i) {
    drops[i].head = -1;
    drops[i].boost = 0;
    for (uint8_t k = 0; k < MAX_TRAIL; ++k) drop_char_idx[i][k] = 0;
    for (uint8_t b = 0; b < BRIGHT_BYTES_PER_DROP + 1; ++b) drop_bright_packed[i][b] = 0;
  }
  for (uint8_t c = 0; c < TERM_COLS; ++c) column_occupied[c] = false;
  for (uint8_t f = 0; f < MAX_FAST; ++f) fasts[f].active = false;
}

// ---------------------- SPAWN crawling drop ----------------------
void maybeSpawnDrop() {
  global_rng = splitmix32(global_rng);
  if ((global_rng & 0xFFu) > SPAWN_CHANCE) return;

  int16_t idx = -1;
  for (uint8_t i = 0; i < MAX_DROPS; ++i) if (drops[i].head == -1) { idx = i; break; }
  if (idx == -1) return;

  global_rng = splitmix32(global_rng);
  uint8_t start_col = (uint8_t)(global_rng % TERM_COLS);
  int16_t free_col = -1;
  for (uint8_t i = 0; i < TERM_COLS; ++i) {
    uint8_t c = (start_col + i) % TERM_COLS;
    if (!column_occupied[c]) { free_col = c; break; }
  }
  if (free_col == -1) return;

  Drop &d = drops[idx];
  global_rng = splitmix32(global_rng);

  d.col = (uint8_t)free_col;
  d.head = 0;
  d.trail = (uint8_t)((global_rng % (MAX_TRAIL - 2u)) + 3u);
  d.speed = (uint8_t)(((global_rng >> 8) % 5u) + 1u);
  d.age = 0;
  d.rng = splitmix32(global_rng);
  d.boost = 0;

  // initialize vector buffers
  for (uint8_t k = 0; k < d.trail; ++k) {
    drop_char_idx[idx][k] = pickIndex(nextRng(d.rng));
    uint8_t b = (k == 0) ? 3u : (k == 1 ? 2u : 1u);
    set_bright_slot((uint8_t)idx, k, b);
  }
  for (uint8_t k = d.trail; k < MAX_TRAIL; ++k) {
    drop_char_idx[idx][k] = 0;
    set_bright_slot((uint8_t)idx, k, 0);
  }

  column_occupied[d.col] = true;
}

// ---------------------- SPAWN fast drop ----------------------
void maybeSpawnFastDrop() {
  global_rng = splitmix32(global_rng);
  if ((global_rng & 0xFFu) > SPAWN_FAST_CHANCE) return;

  global_rng = splitmix32(global_rng);
  uint8_t start_col = (uint8_t)(global_rng % TERM_COLS);

  int16_t col_with_drop = -1;
  for (uint8_t i = 0; i < TERM_COLS; ++i) {
    uint8_t c = (start_col + i) % TERM_COLS;
    if (column_occupied[c]) { col_with_drop = c; break; }
  }
  if (col_with_drop == -1) return;

  int16_t fidx = -1;
  for (uint8_t i = 0; i < MAX_FAST; ++i) if (!fasts[i].active) { fidx = i; break; }
  if (fidx == -1) return;

  FastDrop &f = fasts[fidx];
  f.active = true;
  f.col = (uint8_t)col_with_drop;
  f.row = 0;
  f.speed = 1;
  f.age = 0;
  global_rng = splitmix32(global_rng);
  f.rng = splitmix32(global_rng);
}

// ---------------------- STEP crawling drops (vectorized, decay to 0) ----------------------
void stepDrops() {
  uint8_t tmp_bright[MAX_TRAIL];

  for (uint8_t i = 0; i < MAX_DROPS; ++i) {
    Drop &d = drops[i];
    if (d.head == -1) continue;

    d.age++;
    if (d.age >= d.speed) {
      d.age = 0;
      d.head++;
      d.rng = splitmix32(d.rng);

      // shift char indices down by 1
      if (d.trail > 1) memmove(&drop_char_idx[i][1], &drop_char_idx[i][0], (size_t)(d.trail - 1));
      drop_char_idx[i][0] = pickIndex(nextRng(d.rng));


// In stepDrops()

uint8_t tmp_bright[MAX_TRAIL];
uint8_t new_bright[MAX_TRAIL];

// Step 1: Unpack all brightness values into a temporary array
for (uint8_t k = 0; k < d.trail; ++k) {
  tmp_bright[k] = get_bright_slot(i, k);
}

      // The new head (at index 0) is always the brightest
      set_bright_slot(i, 0, 3u); // Brightest head

      // For the rest of the trail, calculate brightness based on distance from head
      // This creates a smooth fade from head to tail.
      for (uint8_t k = 1; k < d.trail; ++k) {
        // A simple linear decay based on distance from the head (k)
        uint8_t brightness = 3u - (uint8_t)((3.0 * k) / d.trail);
        set_bright_slot(i, k, brightness);
      }

// Step 3: Repack the updated brightness values from the new array
for (uint8_t k = 0; k < d.trail; ++k) {
  set_bright_slot(i, k, new_bright[k]);
}

      // Note: don't decrement boost here; we will decrement per-frame below for consistent duration
    }

    // decrement boost per frame (frame-based lifetime)
    if (d.boost) d.boost--;

    // deactivate when bottom of trail passed last row
    if (d.head - (int16_t)(d.trail - 1) >= (int16_t)TERM_ROWS) {
      column_occupied[d.col] = false;
      d.head = -1;
      d.boost = 0;
      for (uint8_t b = 0; b < BRIGHT_BYTES_PER_DROP + 1; ++b) drop_bright_packed[i][b] = 0;
      for (uint8_t k = 0; k < MAX_TRAIL; ++k) drop_char_idx[i][k] = 0;
    }
  }
}

// ---------------------- STEP fast drops (update vector buffers while passing) ----------------------
void stepFastDrops() {
  for (uint8_t i = 0; i < MAX_FAST; ++i) {
    FastDrop &f = fasts[i];
    if (!f.active) continue;

    f.age++;
    if (f.age >= f.speed) {
      f.age = 0;
      f.row++;
      f.rng = splitmix32(f.rng); // advance fast RNG each step

      if (f.row >= TERM_ROWS) { f.active = false; continue; }

      int di = findDropIndexInColumn(f.col);
      if (di != -1) {
        Drop &d = drops[di];
        int16_t bottom = d.head - (int16_t)(d.trail - 1);

        // while inside visible trail, update that slot's char & bump brightness
        if (f.row >= bottom && f.row <= d.head) {
          //int16_t dist = d.head - f.row;
          int16_t dist = f.row - d.head ; // this is correct
          
          if (dist >= 0 && dist < d.trail) {
            // use fast RNG to generate new char for that position (visible pass-through)
            drop_char_idx[di][dist] = pickIndex(nextRng(f.rng));
            // bump brightness (cap at 3)
            uint8_t cur = get_bright_slot(di, (uint8_t)dist);
            uint8_t nb = (cur < 3u) ? (cur + 1u) : 3u;
            set_bright_slot(di, (uint8_t)dist, nb);
          }
        }

        // stop at bottom and boost head
        if (f.row >= bottom) {
          if (d.boost < 8) d.boost = 8; // boost frames (frame based)
          d.rng = splitmix32(splitmix32(d.rng)); // big RNG jolt
          set_bright_slot((uint8_t)di, 0, 3u); // ensure head slot bright (packed)
          f.active = false;
        }
      }
    }
  }
}

// ---------------------- RENDER ----------------------
void renderFrame(uint32_t salt) {
  Serial.write("\x1b[H");
  uint8_t last_brightness = 255;

  for (uint8_t r = 0; r < TERM_ROWS; ++r) {
    for (uint8_t c = 0; c < TERM_COLS; ++c) {
      uint8_t current_brightness = 0;
      char ch = ' ';
      bool painted = false;

      // Fast-drop visuals first (priority)
      for (uint8_t fi = 0; fi < MAX_FAST; ++fi) {
        const FastDrop &f = fasts[fi];
        if (!f.active || f.col != c) continue;
        if (f.row == (int16_t)r) {
          current_brightness = 4; // super-bright fast head
          ch = charForFast(f);
          painted = true;
          break;
        } else if (f.row - 1 == (int16_t)r) {
          current_brightness = 2; // faint trailing pixel
          ch = charForFast(f);
          painted = true;
          break;
        }
      }

      // Crawling drops (vectorized)
      if (!painted && column_occupied[c]) {
        for (uint8_t di = 0; di < MAX_DROPS; ++di) {
          const Drop &d = drops[di];
          if (d.head == -1 || d.col != c) continue;

          //int16_t dist = d.head - (int16_t)r;
          int16_t dist = (int16_t)r - d.head ; // this is correct (causes entire drop to be drawn when spawned, simulating rain hitting glass)
          
          if (dist >= 0 && dist < d.trail) {
            // overlay boosted head (render-time) if boost active
            if (dist == 0 && d.boost > 0) {
              current_brightness = 4; // super-bright because of boost overlay
            } else {
              current_brightness = get_bright_slot(di, (uint8_t)dist); // 0..3
            }
            uint8_t idx = drop_char_idx[di][dist];
            ch = charset_at_idx(idx);
            painted = true;
            break;
          }
        }
      }

      // minimize escape sequences: change color only when brightness differs
      if (current_brightness != last_brightness) {
        term_set_color(current_brightness);
        last_brightness = current_brightness;
      }
      Serial.write(ch);
    }

    if (r < TERM_ROWS - 1) Serial.write("\r\n");
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
  maybeSpawnFastDrop();

  stepDrops();
  stepFastDrops();

  renderFrame(salt);

  salt = splitmix32(salt);
  delay(FRAME_MS);
}

// ---------------------- CLEANUP ----------------------
void cleanupAndShowCursor() {
  term_show_cursor();
  term_reset_color();
}
