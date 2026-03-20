#include <Arduino.h>

// ---------------------- CONFIG ----------------------
const uint32_t SERIAL_BAUD = 115200;
const uint8_t TERM_COLS = 80;
const uint8_t TERM_ROWS = 24;
const uint16_t FRAME_MS = 30;
const uint8_t MAX_DROPS = 25;
const uint8_t MAX_TRAIL = 12;
const uint8_t SPAWN_CHANCE = 35;
const uint8_t SPAWN_FAST_CHANCE = 18;
const char CHARSET[] = "abcdefghijklmnopqrstuvwxyz0123456789@#$%&*()<>/\"";

// ---------------------- STATE ----------------------
struct Drop {
  uint8_t col;
  int16_t head;
  uint8_t trail;
  uint8_t speed;
  uint8_t age;
  uint32_t rng;
  uint8_t boost;
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
bool column_occupied[TERM_COLS];
uint32_t global_rng = 0xA5A5A5A5u;
const uint8_t MAX_FAST = 12;
FastDrop fasts[MAX_FAST];

// Forward declarations
uint32_t splitmix32(uint32_t x);
uint32_t nextRng(uint32_t &st);
char pickChar(uint32_t h);
void term_clear();
void term_hide_cursor();
void term_show_cursor();
void term_set_color(uint8_t bright);
void term_reset_color();
int findDropIndexInColumn(uint8_t col);
char charForDrop(const Drop &d, int16_t dist);
char charForFast(const FastDrop &f);
void setupState();
void maybeSpawnDrop();
void maybeSpawnFastDrop();
void stepDrops();
void stepFastDrops();
void renderFrame(uint32_t salt);

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
    case 4: Serial.write("\x1b[97;1m"); break;
    case 3: Serial.write("\x1b[97;1m"); break;
    case 2: Serial.write("\x1b[32;1m"); break;
    case 1: Serial.write("\x1b[32m");   break;
    default: Serial.write("\x1b[90m");  break;
  }
}
static inline void term_reset_color() { Serial.write("\x1b[0m"); }

// ---------------------- UTIL ----------------------
int findDropIndexInColumn(uint8_t col) {
  for (uint8_t i = 0; i < MAX_DROPS; ++i) {
    if (drops[i].head != -1 && drops[i].col == col) return i;
  }
  return -1;
}

char charForDrop(const Drop &d, int16_t dist) {
  uint32_t h = d.rng + (uint32_t)dist * 0x9e3779b1u;
  h = splitmix32(h);
  return pickChar(h);
}

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
  int16_t idx = -1;
  for (uint8_t i = 0; i < MAX_DROPS; ++i) if (drops[i].head == -1) { idx = i; break; }
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
  global_rng = splitmix32(global_rng);
  d.col = (uint8_t)free_col;
  d.head = 0;
  d.trail = (global_rng % (MAX_TRAIL - 2)) + 3;
  d.speed = ((global_rng >> 8) % 5) + 1;
  d.age = 0;
  d.rng = splitmix32(global_rng);
  d.boost = 0;
  column_occupied[d.col] = true;
}

void maybeSpawnFastDrop() {
  global_rng = splitmix32(global_rng);
  if ((global_rng & 0xFF) > SPAWN_FAST_CHANCE) return;
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
  f.rng = splitmix32(global_rng);
}

// ---------------------- STEPPERS ----------------------
void stepDrops() {
  for (uint8_t i = 0; i < MAX_DROPS; ++i) {
    Drop &d = drops[i];
    if (d.head == -1) continue;
    d.age++;
    if (d.age >= d.speed) {
      d.age = 0;
      d.head++;
      d.rng = splitmix32(d.rng);
      if (d.boost) {
        uint8_t extra = (d.boost > 4) ? 3 : 1;
        for (uint8_t k = 0; k < extra; ++k) d.rng = splitmix32(d.rng);
      }
      if (d.boost) d.boost--;
    }
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
      f.row++;
      if (f.row >= TERM_ROWS) { f.active = false; continue; }
      int di = findDropIndexInColumn(f.col);
      if (di != -1) {
        Drop &d = drops[di];
        int16_t bottom = d.head - (int16_t)(d.trail - 1);
        if (f.row >= bottom && f.row <= d.head) { d.rng = splitmix32(d.rng); }
        if (f.row >= d.head) {
          if (d.boost < 4) d.boost = 4;
          d.rng = splitmix32(splitmix32(d.rng));
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
      for (uint8_t fi = 0; fi < MAX_FAST; ++fi) {
        const FastDrop &f = fasts[fi];
        if (!f.active || f.col != c) continue;
        if (f.row == (int16_t)r) { current_brightness = 4; ch = charForFast(f); painted = true; break; }
      }
      if (!painted && column_occupied[c]) {
        for (uint8_t di = 0; di < MAX_DROPS; ++di) {
          const Drop &d = drops[di];
          if (d.head == -1 || d.col != c) continue;
          int16_t dist = d.head - (int16_t)r;
          if (dist >= 0 && dist < d.trail) {
            if (dist == 0) {
              if (d.boost) current_brightness = 4;
              else current_brightness = 3;
            } else if (dist < 4) current_brightness = 3 - dist;
            else current_brightness = 1;
            ch = charForDrop(d, dist);
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
