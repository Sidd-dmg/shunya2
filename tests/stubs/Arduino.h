#ifndef TEST_ARDUINO_H
#define TEST_ARDUINO_H

#include <stddef.h>
#include <stdint.h>

#define HIGH 1
#define LOW 0

class String {};

struct TestSerial {
    template <typename... Args> void printf(const char *, Args...) {}
    void println(const char *) {}
};

extern TestSerial Serial;
inline void digitalWrite(uint8_t, uint8_t) {}

#endif
