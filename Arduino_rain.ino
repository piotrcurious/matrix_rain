#include "Arduino.h"

// Forward declarations
uint32_t splitmix32(uint32_t x);
uint32_t nextRng(uint32_t &st);
char pickChar(uint32_t h);
void term_clear();
void term_hide_cursor();
void term_show_cursor();
void term_move(uint8_t r, uint8_t c);
void term_set_color(uint8_t bright);
void term_reset_color();
void setupColumns();
void maybeSpawnHeads();
void stepColumns();
char charFor(uint8_t c, uint8_t r, uint32_t salt);
void renderFrame(uint32_t salt);

// ---------------------- CONFIG ----------------------
const uint32_t SERIAL_BAUD = 115200;
const uint8_t TERM_COLS = 60;
const uint8_t TERM_ROWS = 24;
const uint16_t FRAME_MS = 60;
const uint8_t MAX_TRAIL = min((uint8_t)TERM_ROWS, (uint8_t)12);
const char charset[] = "abcdefghijklmnopqrstuvwxyz0123456789@#$%&*()<>\\/";

// ---------------------- VECTORIZED STATE ----------------------
struct Column {
  int8_t head;
  uint8_t trail;
  uint8_t speed;
  uint8_t age;
  uint32_t rng;
};

Column cols[TERM_COLS];
uint8_t brightness[TERM_COLS][TERM_ROWS];

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
  uint32_t idx = h % (sizeof(charset) - 1);
  return charset[idx];
}

// ---------------------- TERMINAL HELPERS ----------------------
static inline void term_clear() {
  Serial.write("\x1b[2J");
  Serial.write("\x1b[H");
}
static inline void term_hide_cursor() { Serial.write("\x1b[?25l"); }
static inline void term_show_cursor() { Serial.write("\x1b[?25h"); }
static inline void term_move(uint8_t r, uint8_t c) {
  char buf[16];
  snprintf(buf, sizeof(buf), "\x1b[%u;%uH", (unsigned)r, (unsigned)c);
  Serial.write(buf);
}
static inline void term_set_color(uint8_t bright) {
  switch (bright) {
  case 3: Serial.write("\x1b[97;1m"); break;
  case 2: Serial.write("\x1b[32;1m"); break;
  case 1: Serial.write("\x1b[32m"); break;
  default: Serial.write("\x1b[90m"); break;
  }
}

static inline void term_reset_color() { Serial.write("\x1b[0m"); }

// ---------------------- INITIALIZATION ----------------------
void setupColumns() {
  for (uint8_t c = 0; c < TERM_COLS; ++c) {
    uint32_t seed = ((uint32_t)micros()) ^ ((uint32_t)c * 0x9e3779b1u);
    cols[c].rng = splitmix32(seed);
    cols[c].head = -1;
    cols[c].trail = (nextRng(cols[c].rng) % (MAX_TRAIL - 2)) + 3;
    cols[c].speed = (nextRng(cols[c].rng) % 5) + 1;
    cols[c].age = (uint8_t)(nextRng(cols[c].rng) % cols[c].speed);
    for (uint8_t r = 0; r < TERM_ROWS; ++r)
      brightness[c][r] = 0;
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
    for (int r = TERM_ROWS - 1; r >= 0; --r) {
      if (brightness[c][r] > 0)
        brightness[c][r]--;
    }

    if (col.head != -1) {
      col.age++;
      if (col.age >= col.speed) {
        col.age = 0;
        if (col.head < TERM_ROWS) {
          uint8_t hpos = (uint8_t)col.head;
          brightness[c][hpos] = 3;
          for (uint8_t t = 1; t <= col.trail; ++t) {
            int8_t pos = (int8_t)hpos - (int8_t)t;
            if (pos >= 0 && pos < TERM_ROWS) {
              uint8_t b = 3 - (t > 3 ? 3 : t);
              if (b > brightness[c][pos])
                brightness[c][pos] = b;
            }
          }
          col.head++;
        } else {
          uint32_t v = nextRng(col.rng);
          if ((v & 0x7F) < 48) {
            col.head = -1;
          } else {
            col.head = 0;
            col.trail = (v % (MAX_TRAIL - 2)) + 3;
            col.speed = (v >> 8) % 5 + 1;
          }
        }
      }
    }
  }
}

char charFor(uint8_t c, uint8_t r, uint32_t salt) {
  uint32_t h = (uint32_t)c * 0x9e3779b1u + (uint32_t)r * 0x85ebca6bu + salt;
  h = splitmix32(h);
  return pickChar(h);
}

void renderFrame(uint32_t salt) {
  Serial.write("\x1b[H");
  for (uint8_t r = 0; r < TERM_ROWS; ++r) {
    for (uint8_t c = 0; c < TERM_COLS; ++c) {
      uint8_t b = brightness[c][r];
      term_set_color(b);
      char ch = (b == 0) ? ' ' : charFor(c, r, salt + ((uint32_t)r << 8));
      Serial.write(ch);
      term_reset_color();
    }
    Serial.write('\n');
  }
}

void setup() {
  Serial.begin(SERIAL_BAUD);
  uint32_t start = millis();
  while (!Serial && (millis() - start < 1000)) {
    delay(1);
  }
  delay(50);
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
