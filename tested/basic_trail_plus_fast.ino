/*
  arduino_matrix_rain_fastdrops.ino
  Extends matrix rain with "fast" drops that fall through crawling drops,
  change the crawling trail characters as they pass, and boost the crawling head.
*/

#include <Arduino.h>

// ---------------------- CONFIG ----------------------
const uint32_t SERIAL_BAUD = 115200;
const uint8_t TERM_COLS = 80;   // adjust to terminal width if desired
const uint8_t TERM_ROWS = 24;   // adjust to terminal height if desired

const uint16_t FRAME_MS = 30;
const uint8_t MAX_DROPS = 25;
const uint8_t MAX_TRAIL = 12;
const uint8_t SPAWN_CHANCE = 35;     // for crawling drops (out of 256)
const uint8_t SPAWN_FAST_CHANCE = 18; // for fast drops (out of 256)
const char CHARSET[] = "abcdefghijklmnopqrstuvwxyz0123456789@#$%&*()<>/\"";

// ---------------------- STATE ----------------------
struct Drop {
  uint8_t col;      // Column (x-position)
  int16_t head;     // Head row; -1 = inactive
  uint8_t trail;    // Trail length
  uint8_t speed;    // Frames per step (1..5)
  uint8_t age;      // Frame counter
  uint32_t rng;     // Per-drop RNG state
  uint8_t boost;    // boost frames remaining (increases roll freq & visual emphasis)
};

struct FastDrop {
  bool active;
  uint8_t col;
  int16_t row;     // current row (0..TERM_ROWS-1); starts at 0 and increases
  uint8_t speed;   // frames per step (fast = 1 or maybe 0)
  uint8_t age;
  uint32_t rng;
};

Drop drops[MAX_DROPS];
bool column_occupied[TERM_COLS];
uint32_t global_rng = 0xA5A5A5A5u; // seed for spawning

const uint8_t MAX_FAST = 12;
FastDrop fasts[MAX_FAST];

// ---------------------- PRNG ----------------------
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
    case 4: Serial.write("\x1b[97;1m"); break; // super-bright (we map 4->bright white)
    case 3: Serial.write("\x1b[97;1m"); break; // Bright White (head)
    case 2: Serial.write("\x1b[32;1m"); break; // Bright Green
    case 1: Serial.write("\x1b[32m");   break; // Normal Green
    default: Serial.write("\x1b[90m");  break; // Dark Gray (empty)
  }
}
static inline void term_reset_color() { Serial.write("\x1b[0m"); }

// ---------------------- UTIL ----------------------
// find index of crawling drop in a column (or -1)
int findDropIndexInColumn(uint8_t col) {
  for (uint8_t i = 0; i < MAX_DROPS; ++i) {
    if (drops[i].head != -1 && drops[i].col == col) return i;
  }
  return -1;
}

// compute the character for a crawling drop at distance dist from its head
char charForDrop(const Drop &d, int16_t dist) {
  // mix drop RNG with distance so characters roll as the RNG changes
  uint32_t h = d.rng + (uint32_t)dist * 0x9e3779b1u;
  h = splitmix32(h);
  return pickChar(h);
}

// character for a fast drop
char charForFast(const FastDrop &f) {
  uint32_t h = f.rng;
  h = splitmix32(h);
  return pickChar(h);
}

// ---------------------- INIT ----------------------
void setupState() {
  for (uint8_t i = 0; i < MAX_DROPS; ++i) {
    drops[i].head = -1;
    drops[i].boost = 0;
  }
  for (uint8_t c = 0; c < TERM_COLS; ++c) column_occupied[c] = false;

  for (uint8_t i = 0; i < MAX_FAST; ++i) fasts[i].active = false;
}

// ---------------------- SPAWN LOGIC ----------------------
void maybeSpawnDrop() {
  global_rng = splitmix32(global_rng);
  if ((global_rng & 0xFF) > SPAWN_CHANCE) return;

  // inactive slot
  int16_t idx = -1;
  for (uint8_t i = 0; i < MAX_DROPS; ++i) if (drops[i].head == -1) { idx = i; break; }
  if (idx == -1) return;

  // pick a free column
  global_rng = splitmix32(global_rng);
  uint8_t start_col = global_rng % TERM_COLS;
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
  d.trail = (global_rng % (MAX_TRAIL - 2)) + 3; // 3..(MAX_TRAIL)
  d.speed = ((global_rng >> 8) % 5) + 1;       // 1..5
  d.age = 0;
  d.rng = splitmix32(global_rng);
  d.boost = 0;

  column_occupied[d.col] = true;
}

// spawn fast drops but only into columns that have an active crawling drop
void maybeSpawnFastDrop() {
  global_rng = splitmix32(global_rng);
  if ((global_rng & 0xFF) > SPAWN_FAST_CHANCE) return;

  // pick a column that currently has a crawling drop (to ensure interaction)
  global_rng = splitmix32(global_rng);
  uint8_t start_col = global_rng % TERM_COLS;

  int16_t col_with_drop = -1;
  for (uint8_t i = 0; i < TERM_COLS; ++i) {
    uint8_t c = (start_col + i) % TERM_COLS;
    if (column_occupied[c]) { col_with_drop = c; break; }
  }
  if (col_with_drop == -1) return; // no crawling drops present

  // find inactive fast slot
  int16_t fidx = -1;
  for (uint8_t i = 0; i < MAX_FAST; ++i) if (!fasts[i].active) { fidx = i; break; }
  if (fidx == -1) return;

  FastDrop &f = fasts[fidx];
  f.active = true;
  f.col = col_with_drop;
  f.row = 0;                       // top
  f.speed = (global_rng >> 8) % 2 == 0 ? 1 : 1; // very fast (1 frame per step)
  f.age = 0;
  f.rng = splitmix32(global_rng);
}

// ---------------------- STEPPERS ----------------------
void stepDrops() {
  for (uint8_t i = 0; i < MAX_DROPS; ++i) {
    Drop &d = drops[i];
    if (d.head == -1) continue;

    d.age++;
    // if boosted, we also accelerate RNG more often (boost increases rolling "frequency")
    if (d.age >= d.speed) {
      d.age = 0;
      d.head++;                  // move head down
      d.rng = splitmix32(d.rng); // normal advance
      // if boosted, advance rng extra times to make characters roll faster while boosted
      if (d.boost) {
        // extra RNG steps proportional to remaining boost
        uint8_t extra = (d.boost > 4) ? 3 : 1;
        for (uint8_t k = 0; k < extra; ++k) d.rng = splitmix32(d.rng);
      }
      if (d.boost) d.boost = (d.boost > 0) ? d.boost - 1 : 0;
    }

    // deactivate when entire trail passed the bottom of the terminal
    // bottom_of_trail_row = d.head - (d.trail - 1)
    if (d.head - (int16_t)(d.trail - 1) >= (int16_t)TERM_ROWS) {
      column_occupied[d.col] = false;
      d.head = -1;
      d.boost = 0;
    }
  }
}

void stepFastDrops() {
  for (uint8_t i = 0; i < MAX_FAST; ++i) {
    FastDrop &f = fasts[i];
    if (!f.active) continue;

    f.age++;
    if (f.age >= f.speed) {
      f.age = 0;
      f.row++; // fast drop moves down

      // if falling off-screen -> deactivate
      if (f.row >= TERM_ROWS) { f.active = false; continue; }

      // check for interaction with crawling drop in same column
      int di = findDropIndexInColumn(f.col);
      if (di != -1) {
        Drop &d = drops[di];
        // crawling trail covers rows: bottom = d.head - (d.trail - 1)  ... head = d.head
        int16_t bottom = d.head - (int16_t)(d.trail - 1);

        // If fast drop is inside the trail, change the crawling RNG so characters "roll" as it passes
        if (f.row >= bottom && f.row <= d.head) {
          // make the crawling stream change when the fast drop moves through
          d.rng = splitmix32(d.rng);
        }

        // If fast drop reaches the crawling head (bottom-most visual point), boost the head and stop the fast
        if (f.row >= d.head) {
          // Boost: increase rolling frequency for a few frames and give visual emphasis
          if (d.boost < 4) d.boost = 4; // boost for at least 4 frames

          // additional RNG jolt for an instant strong change
          d.rng = splitmix32(splitmix32(d.rng));
          // remove fast drop (it "lands" on the head)
          f.active = false;
        }
      }
    }
  }
}


// ---------------------- RENDER ----------------------
void renderFrame(uint32_t salt) {
  Serial.write("\x1b[H"); // home cursor
  uint8_t last_brightness = 255;

  for (uint8_t r = 0; r < TERM_ROWS; ++r) {
    for (uint8_t c = 0; c < TERM_COLS; ++c) {
      uint8_t current_brightness = 0;
      char ch = ' ';

      // Priority: fast drop visual (if any) > crawling drop trail cell
      bool painted = false;

      // render fast drops first (they look bright)
      for (uint8_t fi = 0; fi < MAX_FAST; ++fi) {
        const FastDrop &f = fasts[fi];
        if (!f.active || f.col != c) continue;
        if (f.row == (int16_t)r) {
          // bright fast head
          current_brightness = 4; // map 4 -> bright white/bold
          ch = charForFast(f);
          painted = true;
          break;
        }
      }

      if (!painted && column_occupied[c]) {
        // find crawling drop in this column
        for (uint8_t di = 0; di < MAX_DROPS; ++di) {
          const Drop &d = drops[di];
          if (d.head == -1 || d.col != c) continue;

          int16_t dist = d.head - (int16_t)r; // 0=head at this row; positive = head below this row
          if (dist >= 0 && dist < d.trail) {
            // brightness: head strong; if boost active make it visually emphasized
            if (dist == 0) {
              // head
              if (d.boost) current_brightness = 4; // boosted head -> super bright
              else current_brightness = 3;
            } else if (dist < 4) current_brightness = 3 - dist; // 2,1
            else current_brightness = 1;

            ch = charForDrop(d, dist);
            painted = true;
            break; // only one crawling drop per column
          }
        }
      }

      if (current_brightness != last_brightness) {
        term_set_color(current_brightness);
        last_brightness = current_brightness;
      }
      Serial.write(ch);
    }

    // use CR+LF so terminals like minicom position the cursor properly
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

  // slowly change salt to give some global variety still (unused for drops RNG)
  salt = splitmix32(salt);
  delay(FRAME_MS);
}

void cleanupAndShowCursor() {
  term_show_cursor();
  term_reset_color();
}
