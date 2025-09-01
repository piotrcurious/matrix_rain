/*
  matrix_rain_vectorized_final.ino
  Fully vectorized, stable Matrix rain with proper fade, bright head,
  full tail on spawn, drop persistence, and fast-drop overlay.
*/

#include <Arduino.h>
#include <string.h>

// ---------------------- CONFIG ----------------------
const uint32_t SERIAL_BAUD = 115200;
const uint8_t TERM_COLS = 80;
const uint8_t TERM_ROWS = 24;
const uint16_t FRAME_MS = 1;

const uint8_t MAX_DROPS = 35;
const uint8_t MAX_TRAIL = 12;
const uint8_t MAX_FAST = 16;

const uint8_t SPAWN_CHANCE = 45;      // chance to spawn a crawling drop
const uint8_t SPAWN_FAST_CHANCE = 28; // chance to spawn a fast drop

const char CHARSET[] = "abcdefghijklmnopqrstuvwxyz0123456789@#$%&*()<>/\"";

// ---------------------- DERIVED ----------------------
#define BRIGHT_BYTES_PER_DROP ((MAX_TRAIL * 2 + 7) / 8)

// ---------------------- STATE ----------------------
struct Drop {
  uint8_t col;
  int16_t head_row;   // top row of drop
  uint8_t trail;
  uint8_t speed;
  uint8_t age;
  uint32_t rng;
  uint8_t boost;      // frames remaining
};

struct FastDrop {
  bool active;
  uint8_t col;
  int16_t row;
  uint8_t speed;
  uint8_t age;
  uint32_t rng;
};

Drop drops[MAX_DROPS];
FastDrop fasts[MAX_FAST];
bool column_occupied[TERM_COLS];
uint32_t global_rng = 0xA5A5A5A5u;

// Vector buffers
static uint8_t drop_char_idx[MAX_DROPS][MAX_TRAIL];
static uint8_t drop_bright_packed[MAX_DROPS][BRIGHT_BYTES_PER_DROP + 1];

// ---------------------- PRNG ----------------------
static inline uint32_t splitmix32(uint32_t x) {
  x += 0x9e3779b9u;
  x = (x ^ (x >> 16)) * 0x85ebca6bu;
  x = (x ^ (x >> 13)) * 0xc2b2ae35u;
  return x ^ (x >> 16);
}

static inline uint32_t nextRng(uint32_t &st) {
  st = splitmix32(st);
  return st;
}

static inline uint8_t pickIndex(uint32_t h) {
  return (uint8_t)(h % (sizeof(CHARSET) - 1));
}

// ---------------------- Terminal helpers ----------------------
static inline void term_clear() { Serial.write("\x1b[2J"); Serial.write("\x1b[H"); }
static inline void term_hide_cursor() { Serial.write("\x1b[?25l"); }
static inline void term_show_cursor() { Serial.write("\x1b[?25h"); }

//static inline void term_set_color(uint8_t bright) {
//  switch (bright) {
//    case 4: Serial.write("\x1b[97;1m"); break; // super-bright white
//    case 3: Serial.write("\x1b[97m"); break;   // bright white
//    case 2: Serial.write("\x1b[32;1m"); break; // bright green
//    case 1: Serial.write("\x1b[32m"); break;   // normal green
//    default: Serial.write("\x1b[90m"); break;  // dim / off
//  }
//}


//// Corrected code for VT102
//static inline void term_set_color(uint8_t bright) {
//  switch (bright) {
//    case 4:
//    case 3: Serial.write("\x1b[37m"); break; // white (no "bright" option)
//    case 2:
//    case 1: Serial.write("\x1b[32m"); break; // green (no "bright" option)
//    default: Serial.write("\x1b[90m"); break; // dim gray / off
//  }
//}

static inline void term_set_color(uint8_t bright) {
  switch (bright) {
    case 4: Serial.write("\x1b[37m"); break;  // white
    case 3: Serial.write("\x1b[32m"); break;  // green
    case 2: Serial.write("\x1b[33m"); break;  // yellow
    case 1:
    default: Serial.write("\x1b[31m"); break; // grey (non-standard but works on emulators)
  }
}

static inline void term_reset_color() { Serial.write("\x1b[0m"); }

// ---------------------- Packed brightness ----------------------
static inline uint8_t get_bright_slot(uint8_t drop_idx, uint8_t slot) {
  uint16_t bitpos = slot * 2;
  uint8_t byteIdx = bitpos / 8;
  uint8_t bitOff = bitpos % 8;
  uint16_t val = drop_bright_packed[drop_idx][byteIdx] | ((uint16_t)drop_bright_packed[drop_idx][byteIdx + 1] << 8);
  return (uint8_t)((val >> bitOff) & 0x03u);
}

static inline void set_bright_slot(uint8_t drop_idx, uint8_t slot, uint8_t val) {
  val &= 0x03u;
  uint16_t bitpos = slot * 2;
  uint8_t byteIdx = bitpos / 8;
  uint8_t bitOff = bitpos % 8;
  uint16_t mask = (uint16_t)~(0x03u << bitOff);
  uint16_t data = drop_bright_packed[drop_idx][byteIdx] | ((uint16_t)drop_bright_packed[drop_idx][byteIdx + 1] << 8);
  data = (data & mask) | ((uint16_t)val << bitOff);
  drop_bright_packed[drop_idx][byteIdx] = (uint8_t)data;
  drop_bright_packed[drop_idx][byteIdx + 1] = (uint8_t)(data >> 8);
}

// ---------------------- Utilities ----------------------
int findDropIndexInColumn(uint8_t col) {
  for (uint8_t i = 0; i < MAX_DROPS; ++i)
    if (drops[i].head_row != -1 && drops[i].col == col) return i;
  return -1;
}

static inline char charset_at_idx(uint8_t idx) {
  return CHARSET[idx % (sizeof(CHARSET) - 1)];
}

static inline char charForFast(const FastDrop &f) {
  return charset_at_idx((uint8_t)(f.rng % (sizeof(CHARSET) - 1)));
}

// ---------------------- INIT ----------------------
void setupState() {
  for (uint8_t i = 0; i < MAX_DROPS; ++i) {
    drops[i].head_row = -1;
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
  for (uint8_t i = 0; i < MAX_DROPS; ++i) if (drops[i].head_row == -1) { idx = i; break; }
  if (idx == -1) return;

  global_rng = splitmix32(global_rng);
  uint8_t start_col = global_rng % TERM_COLS;
  int16_t free_col = -1;
  for (uint8_t i = 0; i < TERM_COLS; ++i) {
    uint8_t c = (start_col + i) % TERM_COLS;
    if (!column_occupied[c]) { free_col = c; break; }
  }
  if (free_col == -1) return;

  Drop &d = drops[idx];
  d.col = free_col;
  d.head_row = 0;
  d.trail = (global_rng % (MAX_TRAIL - 2u)) + 3u;
  d.speed = ((global_rng >> 8) % 5u) + 1u;
  d.age = 0;
  d.rng = splitmix32(global_rng);
  d.boost = 0;

  // Initialize vector buffers: head→tail brightness decreasing
  for (uint8_t k = 0; k < d.trail; ++k) {
    drop_char_idx[idx][k] = pickIndex(nextRng(d.rng));
    uint8_t b = (k == 0) ? 4 : (k == 1 ? 3 : 1);
    set_bright_slot(idx, k, b);
  }
  for (uint8_t k = d.trail; k < MAX_TRAIL; ++k) {
    drop_char_idx[idx][k] = 0;
    set_bright_slot(idx, k, 0);
  }

  column_occupied[d.col] = true;
}

// ---------------------- SPAWN fast drop ----------------------
void maybeSpawnFastDrop() {
  global_rng = splitmix32(global_rng);
  if ((global_rng & 0xFFu) > SPAWN_FAST_CHANCE) return;

  global_rng = splitmix32(global_rng);
  uint8_t start_col = global_rng % TERM_COLS;

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
  f.col = col_with_drop;
  f.row = 0;
  f.speed = 1;
  f.age = 0;
  global_rng = splitmix32(global_rng);
  f.rng = splitmix32(global_rng);
}

// ---------------------- STEP crawling drops ----------------------
void stepDrops() {
  for (uint8_t i = 0; i < MAX_DROPS; ++i) {
    Drop &d = drops[i];
    if (d.head_row == -1) continue;

    d.age++;
    if (d.age >= d.speed) {
      d.age = 0;
      d.head_row++;
      d.rng = splitmix32(d.rng);

      // Generate new char for head
      for (uint8_t k = d.trail - 1; k > 0; --k) {
        drop_char_idx[i][k] = drop_char_idx[i][k - 1];
 //       uint8_t val = get_bright_slot(i, k - 1);
        uint8_t val = 4;
 
        set_bright_slot(i, k, val);
      }
      drop_char_idx[i][0] = pickIndex(nextRng(d.rng));

      // Fade all slots down by 1 (head may be boosted temporarily)
      for (uint8_t k = d.trail - 1; k > 0; --k) {
        uint8_t val = get_bright_slot(i, k - 1);
        set_bright_slot(i, k, val);
      }
      // Head slot (0)
      uint8_t head_val = get_bright_slot(i, 0);
      if (d.boost > 0) head_val = 4; // overlay super-bright
      set_bright_slot(i, 0, head_val);
    }

    if (d.boost) d.boost--;

    // Deactivate only when all slots are zero and head past bottom
    bool all_faded = true;
    for (uint8_t k = 0; k < d.trail; ++k) if (get_bright_slot(i, k) > 0) all_faded = false;
    if (all_faded && d.head_row >= TERM_ROWS) {
      column_occupied[d.col] = false;
      d.head_row = -1;
      d.boost = 0;
    }
  }
}

// ---------------------- STEP fast drops ----------------------
void stepFastDrops() {
  for (uint8_t i = 0; i < MAX_FAST; ++i) {
    FastDrop &f = fasts[i];
    if (!f.active) continue;

    f.age++;
    if (f.age >= f.speed) {
      f.age = 0;
      f.row++;
      f.rng = splitmix32(f.rng);

      if (f.row >= TERM_ROWS) { f.active = false; continue; }

      int di = findDropIndexInColumn(f.col);
      if (di != -1) {
        Drop &d = drops[di];
        int16_t slot = f.row - d.head_row;
        if (slot >= 0 && slot < d.trail) {
          drop_char_idx[di][slot] = pickIndex(nextRng(f.rng));
          uint8_t b = get_bright_slot(di, slot);
          if (b < 3) set_bright_slot(di, slot, b + 1);
        }

        if (slot == 0) { // Only boost if it hits the head of the crawling drop
          if (d.boost < 8) d.boost = 8;
          d.rng = splitmix32(splitmix32(d.rng));
          set_bright_slot(di, 0, 4);
          f.active = false;
        }
      }
    }
  }
}

// ---------------------- RENDER ----------------------
void renderFrame() {
  Serial.write("\x1b[H");
  uint8_t last_brightness = 255;

   term_set_color(2);
     
  for (uint8_t r = 0; r < TERM_ROWS; ++r) {
    for (uint8_t c = 0; c < TERM_COLS; ++c) {
      char ch = ' ';
      uint8_t brightness = 0;

      // Fast drops overlay
      for (uint8_t fi = 0; fi < MAX_FAST; ++fi) {
        const FastDrop &f = fasts[fi];
        if (!f.active || f.col != c) continue;
        if (f.row == r) { ch = charForFast(f); brightness = 4; break; }
        if (f.row - 1 == r) { ch = charForFast(f); brightness = 3; break; }
      }
            
      // Crawling drops
      if (brightness == 0 && column_occupied[c]) {

        for (uint8_t di = 0; di < MAX_DROPS; ++di) {
          const Drop &d = drops[di];
          if (d.head_row == -1 || d.col != c) continue;
          int16_t slot = r - d.head_row;
          if (slot >= 0 && slot < d.trail) {
            ch = charset_at_idx(drop_char_idx[di][slot]);
            //brightness = get_bright_slot(di, slot);
            brightness = map(slot,d.trail,0,3,1);
            brightness = constrain (brightness,0,3); 
            // This is now redundant because boost is handled in stepDrops()
            // if (slot == 0 && d.boost > 0) brightness = 4;
            if (slot == d.trail-1) {brightness = 4;}; // the drop itself
            break;
          }
        }
      }

      if (brightness != last_brightness) {
        term_set_color(brightness);
//        term_set_color(4);
        last_brightness = brightness;
      }
 //       term_set_color(4);

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
  while (!Serial && (millis() - start < 1000)) delay(1);

  delay(50);
  term_hide_cursor();
  term_clear();
  setupState();
}

void loop() {
  maybeSpawnDrop();
  maybeSpawnFastDrop();
  stepDrops();
  stepFastDrops();
  renderFrame();
  delay(FRAME_MS);
}

// ---------------------- CLEANUP ----------------------
void cleanupAndShowCursor() {
  term_show_cursor();
  term_reset_color();
}
