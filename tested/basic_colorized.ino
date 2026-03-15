#include "Arduino.h"
#include <avr/pgmspace.h>

// Forward declarations
void term_clear();
void term_move(uint8_t r, uint8_t c);
void term_set_color(uint8_t bright);
void term_reset_color();
void setupColumns();
void maybeSpawnHeads();
void stepColumns();
void renderFrame();

// ---------------------- CONFIG ----------------------
const uint32_t SERIAL_BAUD = 115200;
const uint8_t TERM_COLS = 60;
const uint8_t TERM_ROWS = 24;
const uint16_t FRAME_MS = 60;

// ---------------------- STATE ----------------------
struct Column {
  int8_t head;
  uint8_t trail;
};

Column cols[TERM_COLS];

// ---------------------- TERMINAL HELPERS ----------------------
void term_clear() { Serial.print(F("\x1b[2J\x1b[H")); }
void term_move(uint8_t r, uint8_t c) {
  Serial.print(F("\x1b["));
  Serial.print((unsigned)r);
  Serial.print(F(";"));
  Serial.print((unsigned)c);
  Serial.print(F("H"));
}
void term_set_color(uint8_t bright) {
  if (bright == 3)
    Serial.print(F("\x1b[97;1m"));
  else if (bright == 2)
    Serial.print(F("\x1b[32;1m"));
  else if (bright == 1)
    Serial.print(F("\x1b[32m"));
  else
    Serial.print(F("\x1b[90m"));
}
void term_reset_color() { Serial.print(F("\x1b[0m")); }

// ---------------------- INITIALIZATION ----------------------
void setupColumns() {
  for (uint8_t c = 0; c < TERM_COLS; ++c) {
    cols[c].head = -1;
    cols[c].trail = random(3, 12);
  }
}

void maybeSpawnHeads() {
  for (uint8_t c = 0; c < TERM_COLS; ++c) {
    if (cols[c].head == -1 && random(0, 100) < 5) {
      cols[c].head = 0;
      cols[c].trail = random(3, 12);
    }
  }
}

void stepColumns() {
  for (uint8_t c = 0; c < TERM_COLS; ++c) {
    if (cols[c].head != -1) {
      cols[c].head++;
      if (cols[c].head >= TERM_ROWS + cols[c].trail) {
        cols[c].head = -1;
      }
    }
  }
}

void renderFrame() {
  term_move(1, 1);
  for (uint8_t r = 0; r < TERM_ROWS; ++r) {
    for (uint8_t c = 0; c < TERM_COLS; ++c) {
      uint8_t b = 0;
      int dist = (int)cols[c].head - (int)r;
      if (dist >= 0 && dist < cols[c].trail) {
        if (dist == 0)
          b = 3;
        else if (dist < 3)
          b = 2;
        else
          b = 1;
      }
      term_set_color(b);
      Serial.print(b ? (char)random(33, 126) : ' ');
    }
    Serial.println();
  }
}

void setup() {
  Serial.begin(SERIAL_BAUD);
  term_clear();
  setupColumns();
}

void loop() {
  maybeSpawnHeads();
  stepColumns();
  renderFrame();
  delay(FRAME_MS);
}
