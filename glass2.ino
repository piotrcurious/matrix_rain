#include <Arduino.h>
#include <avr/pgmspace.h>

// Terminal size (adjust to your terminal)
const int term_cols = 60;
const int term_rows = 20;

// Background ASCII art (20 rows x 60 cols)
const char ascii_bg[20][61] PROGMEM = {
  "          _____   ______  _____  _______ ______               ",
  "    /\\   |  __ \\ |  ____||  __ \\|__   __||  ____|            ",
  "   /  \\  | |__) || |__   | |  | |  | |   | |__               ",
  "  / /\\ \\ |  _  / |  __|  | |  | |  | |   |  __|              ",
  " / ____ \\| | \\ \\ | |____ | |__| |  | |   | |____             ",
  "/_/    \\_\\_|  \\_\\|______||_____/   |_|   |______|            ",
  "                                                              ",
  "            ... MATRIX GLASS SIMULATION ...                   ",
  "                                                              ",
  "   ~~~   ~~~   ~~~   ~~~   ~~~   ~~~   ~~~   ~~~   ~~~        ",
  "                                                              ",
  "       fractal minds in rain - v2                             ",
  "                                                              ",
  "  [ + ]  [ + ]  [ + ]  [ + ]  [ + ]  [ + ]  [ + ]  [ + ]      ",
  "                                                              ",
  "   .   .   .   .   .   .   .   .   .   .   .   .   .   .      ",
  "  ... ... ... ... ... ... ... ... ... ... ... ... ... ...     ",
  "                                                              ",
  "--------------------------------------------------------------",
  "      S I M U L A T I O N    A C T I V E                      "
};

// Forward declarations
uint32_t hash32(uint32_t x);
char getBgChar(int x, int y, uint32_t t);
bool rainActiveAt(int x, int y, uint32_t frame);
void updateRain(uint32_t frame);
void renderGlassRain(uint32_t frame);
void term_clear();
void term_move(int row, int col);

uint32_t hash32(uint32_t x) {
  x ^= x >> 13;
  x *= 0x5bd1e995;
  x ^= x >> 15;
  return x;
}

char getBgChar(int x, int y, uint32_t t) {
  uint32_t h = hash32((uint32_t)(x + y * term_cols) + t * 31);
  int dx = (int)((h >> 3) & 0x3) - 1;
  int dy = (int)((h >> 7) & 0x3) - 1;

  int nx = (x + dx + term_cols) % term_cols;
  int ny = (y + dy + term_rows) % term_rows;

  return (char)pgm_read_byte(&ascii_bg[ny][nx]);
}

const int max_cols = 60;
uint8_t drops[max_cols];
const uint8_t rain_colors[4] PROGMEM = {46, 82, 118, 154};

void term_clear() { Serial.print(F("\x1b[2J\x1b[H")); }

void term_move(int row, int col) {
  Serial.print(F("\x1b["));
  Serial.print(row + 1);
  Serial.print(F(";"));
  Serial.print(col + 1);
  Serial.print(F("H"));
}

bool rainActiveAt(int x, int y, uint32_t frame) {
  return (drops[x] != 255 && y == drops[x]);
}

void updateRain(uint32_t frame) {
  for (int i = 0; i < term_cols; i++) {
    if (drops[i] == 255) {
      if (random(0, 15) == 0) drops[i] = 0;
    } else {
      if (++drops[i] >= term_rows) drops[i] = 255;
    }
  }
}

void renderGlassRain(uint32_t frame) {
  for (int y = 0; y < term_rows; y++) {
    term_move(y, 0);
    for (int x = 0; x < term_cols; x++) {
      char base = getBgChar(x, y, frame / 4);
      if (rainActiveAt(x, y, frame)) {
        uint8_t c = pgm_read_byte(&rain_colors[(frame / 2 + x) & 3]);
        Serial.print(F("\x1b[38;5;"));
        Serial.print(c);
        Serial.print(F("m"));
        Serial.write(base);
      } else {
        Serial.print(F("\x1b[38;5;241m"));
        Serial.write(base);
      }
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(200);
  term_clear();
  for (int i = 0; i < max_cols; i++) drops[i] = 255;
}

void loop() {
  static uint32_t frame = 0;
  updateRain(frame);
  renderGlassRain(frame);
  frame++;
  delay(80);
}
