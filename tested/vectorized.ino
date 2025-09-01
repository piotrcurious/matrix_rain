/*
  matrix_rain_vectorized.ino

  Memory-efficient Matrix rain with persistent fade using vectorized buffers.
  - Packed 2-bit brightness per slot (0..3)
  - Compact per-drop char index arrays
  - Fast drops that pass through crawling drops and nudge their trail
  - CR+LF + \x1b[H rendering for vt102/minicom compatibility
*/

#include <Arduino.h>
#include <string.h>

// ---------------------- CONFIG ----------------------
const uint32_t SERIAL_BAUD = 115200;

// Terminal size (adjust to your terminal, e.g. 82x26)
const uint8_t TERM_COLS = 80;
const uint8_t TERM_ROWS = 24;

// Visual / performance knobs
const uint16_t FRAME_MS = 10;
const uint8_t MAX_DROPS = 35;
const uint8_t MAX_TRAIL = 12;      // keep small for RAM; 12 is a sensible default
const uint8_t MAX_FAST = 12;       // max simultaneous fast drops
const uint8_t SPAWN_CHANCE = 45;   // (0..255) chance to spawn crawl drop per frame
const uint8_t SPAWN_FAST_CHANCE = 28; // chance to spawn fast drop per frame

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
  uint8_t boost;    // boost frames remaining (increases roll)
};

// Fast "bullet" drop
struct FastDrop {
  bool active;
  uint8_t col;
  int16_t row;   // current row
  uint8_t speed; // frames per step (fast = 1)
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
  // bright: 4 = super-bright (fast or boosted), 3=head, 2=bright green, 1=normal green, 0=dim/blank
  switch (bright) {
    case 4: Serial.write("\x1b[97;1m"); break; // super-bright white
    case 3: Serial.write("\x1b[97;1m"); break; // bright white (head)
    case 2: Serial.write("\x1b[32;1m"); break; // bright green
    case 1: Serial.write("\x1b[32m");   break; // normal green
    default: Serial.write("\x1b[90m");  break; // dim gray / off
  }
}
static inline void term_reset_color() { Serial.write("\x1b[0m"); }

// ---------------------- packed brightness helpers (2 bits per slot) ----------------------
static inline uint8_t get_bright_slot(uint8_t drop_idx, uint8_t slot) {
  // slot 0..MAX_TRAIL-1
  uint16_t bitpos = (uint16_t)slot * 2u;
  uint8_t byteIdx = (uint8_t)(bitpos >> 3);
  uint8_t bitOff = (uint8_t)(bitpos & 7u);
  uint16_t v = drop_bright_packed[drop_idx][byteIdx] >> bitOff;
  if (bitOff <= 6) {
    return (uint8_t)(v & 0x03u);
  } else {
    // spans two bytes: combine low bits with next byte
    uint8_t next = drop_bright_packed[drop_idx][byteIdx + 1];
    v |= (uint16_t)(next << (8 - bitOff));
    return (uint8_t)(v & 0x03u);
  }
}

static inline void set_bright_slot(uint8_t drop_idx, uint8_t slot, uint8_t val) {
  val &= 0x03u;
  uint16_t bitpos = (uint16_t)slot * 2u;
  uint8_t byteIdx = (uint8_t)(bitpos >> 3);
  uint8_t bitOff = (uint8_t)(bitpos & 7u);

  if (bitOff <= 6) {
    uint8_t mask = (uint8_t)(0x03u << bitOff);
    drop_bright_packed[drop_idx][byteIdx] = (drop_bright_packed[drop_idx][byteIdx] & ~mask)
                                           | (uint8_t)(val << bitOff);
  } else {
    // spanning: write low bit to current byte and high bit to next
    // bitOff == 7 only possible in this packing
    uint8_t low_mask = (uint8_t)(1u << 7);
    drop_bright_packed[drop_idx][byteIdx] = (drop_bright_packed[drop_idx][byteIdx] & ~low_mask)
                                           | (uint8_t)((val & 0x01u) << 7);
    uint8_t high_mask = 0x01u;
    drop_bright_packed[drop_idx][byteIdx + 1] = (drop_bright_packed[drop_idx][byteIdx + 1] & ~high_mask)
                                               | (uint8_t)((val >> 1) & 0x01u);
  }
}

// ---------------------- utility ----------------------
int findDropIndexInColumn(uint8_t col) {
  for (uint8_t i = 0; i < MAX_DROPS; ++i) {
    if (drops[i].head != -1 && drops[i].col == col) return (int)i;
  }
  return -1;
}

char charset_at_idx(uint8_t idx) {
  return CHARSET[idx % (sizeof(CHARSET) - 1)];
}

char charForFast(const FastDrop &f) {
  uint32_t h = f.rng;
  h = splitmix32(h);
  return charset_at_idx((uint8_t)(h % (sizeof(CHARSET) - 1)));
}

// ---------------------- INIT ----------------------
void setupState() {
  for (uint8_t i = 0; i < MAX_DROPS; ++i) {
    drops[i].head = -1;
    drops[i].boost = 0;
    // clear buffers
    for (uint8_t k = 0; k < MAX_TRAIL; ++k) drop_char_idx[i][k] = 0;
    for (uint8_t b = 0; b < BRIGHT_BYTES_PER_DROP + 1; ++b) drop_bright_packed[i][b] = 0;
  }
  for (uint8_t c = 0; c < TERM_COLS; ++c) column_occupied[c] = false;
  for (uint8_t f = 0; f < MAX_FAST; ++f) fasts[f].active = false;
}

// ---------------------- SPAWN (crawling drops) ----------------------
void maybeSpawnDrop() {
  global_rng = splitmix32(global_rng);
  if ((global_rng & 0xFFu) > SPAWN_CHANCE) return;

  int16_t idx = -1;
  for (uint8_t i = 0; i < MAX_DROPS; ++i) {
    if (drops[i].head == -1) { idx = i; break; }
  }
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

  // initialize vectorized buffers for this drop
  for (uint8_t k = 0; k < d.trail; ++k) {
    drop_char_idx[idx][k] = pickIndex(nextRng(d.rng));
    uint8_t b = (k == 0) ? 3u : (k == 1 ? 2u : 1u);
    set_bright_slot((uint8_t)idx, k, b);
  }
  // clear rest
  for (uint8_t k = d.trail; k < MAX_TRAIL; ++k) {
    drop_char_idx[idx][k] = 0;
    set_bright_slot((uint8_t)idx, k, 0);
  }

  column_occupied[d.col] = true;
}

// ---------------------- SPAWN (fast drops) ----------------------
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

// ---------------------- STEP crawling drops (vectorized) ----------------------
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

      // shift char indices down by one (memmove)
      if (d.trail > 1) memmove(&drop_char_idx[i][1], &drop_char_idx[i][0], (size_t)(d.trail - 1));
      drop_char_idx[i][0] = pickIndex(nextRng(d.rng));

      // unpack brightness into tmp_bright
      for (uint8_t k = 0; k < d.trail; ++k) tmp_bright[k] = get_bright_slot(i, k);

      // shift & decay brightness
      for (int k = d.trail - 1; k >= 1; --k) {
        uint8_t prev = tmp_bright[k - 1];
        tmp_bright[k] = (prev > 1) ? (prev - 1) : 1;
      }
      // new head brightness (boosted or not) - cap at 3
      tmp_bright[0] = (d.boost ? 3u : 3u);

      // repack back
      for (uint8_t k = 0; k < d.trail; ++k) set_bright_slot(i, k, tmp_bright[k]);

      if (d.boost) d.boost--;
    }

    // deactivate when off-screen (bottom of trail passed)
    if (d.head - (int16_t)(d.trail - 1) >= (int16_t)TERM_ROWS) {
      column_occupied[d.col] = false;
      d.head = -1;
      d.boost = 0;
      // clear packed brightness bytes for safety
      for (uint8_t b = 0; b < BRIGHT_BYTES_PER_DROP + 1; ++b) drop_bright_packed[i][b] = 0;
    }
  }
}

// ---------------------- STEP fast drops (update vectorized buffers) ----------------------
void stepFastDrops() {
  for (uint8_t i = 0; i < MAX_FAST; ++i) {
    FastDrop &f = fasts[i];
    if (!f.active) continue;

    f.age++;
    if (f.age >= f.speed) {
      f.age = 0;
      f.row++;

      if (f.row >= TERM_ROWS) { f.active = false; continue; }

      int di = findDropIndexInColumn(f.col);
      if (di != -1) {
        Drop &d = drops[di];
        int16_t bottom = d.head - (int16_t)(d.trail - 1);

        // pass-through: update char index & bump brightness for that slot
        if (f.row >= bottom && f.row <= d.head) {
          int16_t dist = d.head - f.row;
          if (dist >= 0 && dist < d.trail) {
            drop_char_idx[di][dist] = pickIndex(nextRng(d.rng));
            uint8_t cur = get_bright_slot(di, (uint8_t)dist);
            uint8_t nb = (cur < 3) ? (cur + 1) : 3;
            set_bright_slot(di, (uint8_t)dist, nb);
          }
        }

        // stop at bottom and boost head
        if (f.row >= bottom) {
          if (d.boost < 6) d.boost = 6;
          d.rng = splitmix32(splitmix32(d.rng));
          set_bright_slot((uint8_t)di, 0, 3);
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

      // Fast-drop visuals first
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

      // Crawling drops (vectorized buffers)
      if (!painted && column_occupied[c]) {
        for (uint8_t di = 0; di < MAX_DROPS; ++di) {
          const Drop &d = drops[di];
          if (d.head == -1 || d.col != c) continue;

          int16_t dist = d.head - (int16_t)r;
          if (dist >= 0 && dist < d.trail) {
            current_brightness = get_bright_slot(di, (uint8_t)dist);
            uint8_t idx = drop_char_idx[di][dist];
            ch = charset_at_idx(idx);
            painted = true;
            break;
          }
        }
      }

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
