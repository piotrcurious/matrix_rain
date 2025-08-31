/* Arduino Glass Rain ASCII Demo

Matrix-style rain that distorts an underlying ASCII art image with a glass/refraction effect using hashing + vectorization. Output over Serial as VT102 ANSI escape sequences.

Connect with an ANSI-capable terminal @ 115200 baud. */

#include <Arduino.h> #include <avr/pgmspace.h>

// Terminal size (adjust to your terminal) const int term_cols = 40; const int term_rows = 12;

// Background ASCII art (12 rows x 40 cols) const char ascii_bg[12][41] PROGMEM = { "         OPENAI MATRIX DEMO          ", "                                      ", "     ASCII GLASS DISTORTION           ", "                                      ", "            (VT102)                   ", "                                      ", "   ~~~   ~~~   ~~~   ~~~   ~~~        ", "                                      ", "       fractal minds in rain          ", "                                      ", "                                      ", "--------------------------------------" };

// Hash function for distortion (xorshift-inspired) uint32_t hash32(uint32_t x) { x ^= x >> 13; x *= 0x5bd1e995; x ^= x >> 15; return x; }

// Return background character with distortion applied char getBgChar(int x, int y, uint32_t t) { uint32_t h = hash32((uint32_t)(x + yterm_cols) + t31); int dx = (int)((h >> 3) & 0x3) - 1; // -1..+2 offset int dy = (int)((h >> 7) & 0x3) - 1;

int nx = (x + dx + term_cols) % term_cols; int ny = (y + dy + term_rows) % term_rows;

return (char)pgm_read_byte(&ascii_bg[ny][nx]); }

// Rain state const int max_cols = 40; uint8_t drops[max_cols];

// Palette (basic green shades, could extend with RTC time-based scheme) const uint8_t rain_colors[4] PROGMEM = {40, 82, 118, 154};

void term_clear() { Serial.print(F("\033[2J\033[H")); }

void term_move(int row, int col) { Serial.print(F("\033[")); Serial.print(row+1); Serial.print(F(";")); Serial.print(col+1); Serial.print(F("H")); }

bool rainActiveAt(int x, int y, uint32_t frame) { return (drops[x] != 255 && y == drops[x]); }

void updateRain(uint32_t frame) { for (int i=0; i<term_cols; i++) { if (drops[i] == 255) { if (random(0,20) == 0) drops[i] = 0; // spawn } else { if (++drops[i] >= term_rows) drops[i] = 255; } } }

void renderGlassRain(uint32_t frame) { for (int y=0; y<term_rows; y++) { term_move(y,0); for (int x=0; x<term_cols; x++) { char base = getBgChar(x, y, frame/3); if (rainActiveAt(x,y,frame)) { uint8_t c = pgm_read_byte(&rain_colors[(frame/2 + x) & 3]); Serial.print(F("\033[38;5;")); Serial.print(c); Serial.print(F("m")); Serial.write(base); } else { Serial.print(F("\033[38;5;240m")); Serial.write(base); } } } }

void setup() { Serial.begin(115200); delay(200); term_clear(); for (int i=0;i<max_cols;i++) drops[i] = 255; }

void loop() { static uint32_t frame = 0; updateRain(frame); renderGlassRain(frame); frame++; delay(100); }

