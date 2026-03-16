#ifndef ARDUINO_H
#define ARDUINO_H
#include <iostream>
#include <string>
#include <chrono>
#include <thread>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <cmath>
#define PROGMEM
#define F(s) (s)
using std::min;
using std::max;
typedef std::string String;
class SerialMock {
public:
    void begin(unsigned long b) {}
    void write(char c) { std::cout << c << std::flush; }
    void write(const char* s) { std::cout << s << std::flush; }
    void write(const uint8_t* b, size_t l) { std::cout.write((const char*)b, l); std::cout.flush(); }
    void print(const char* s) { std::cout << s << std::flush; }
    void print(const std::string& s) { std::cout << s << std::flush; }
    void print(int n) { std::cout << n << std::flush; }
    void print(unsigned int n) { std::cout << n << std::flush; }
    void print(long n) { std::cout << n << std::flush; }
    void print(unsigned long n) { std::cout << n << std::flush; }
    void print(double n) { std::cout << n << std::flush; }
    void println(const char* s) { std::cout << s << std::endl; }
    void println(int n) { std::cout << n << std::endl; }
    void println(unsigned int n) { std::cout << n << std::endl; }
    void println(long n) { std::cout << n << std::endl; }
    void println(unsigned long n) { std::cout << n << std::endl; }
    void println() { std::cout << std::endl; }
    operator bool() { return true; }
};
extern SerialMock Serial;
inline unsigned long millis() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
inline unsigned long micros() { return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
inline void delay(unsigned long ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
inline long random(long min, long max) { if (min >= max) return min; return min + (std::rand() % (max - min)); }
inline long random(long max) { return random(0, max); }
inline long map(long x, long in_min, long in_max, long out_min, long out_max) {
  if (in_max == in_min) return out_min;
  return (x - in_min) * (out_max - out_min) / (in_max - in_min) + out_min;
}
template<class T, class L, class H>
inline T constrain(T value, L low, H high) {
  return (value < low) ? (T)low : ((value > high) ? (T)high : value);
}

// Support for Wire mock if needed (minimal)
class WireMock {
public:
    bool begin() { return true; }
};
extern WireMock Wire;

void setup();
void loop();
#include "avr/pgmspace.h"
#endif
