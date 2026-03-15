#ifndef PGMSPACE_H
#define PGMSPACE_H
#include <cstdint>
#include <cstring>
#define PROGMEM
#define pgm_read_byte(ptr) (*(const uint8_t*)(ptr))
#define pgm_read_word(ptr) (*(const uint16_t*)(ptr))
#define pgm_read_dword(ptr) (*(const uint32_t*)(ptr))
#define pgm_read_float(ptr) (*(const float*)(ptr))
#define pgm_read_ptr(ptr) (*(const void**)(ptr))
#endif
