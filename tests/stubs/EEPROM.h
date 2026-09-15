#ifndef TEST_EEPROM_H
#define TEST_EEPROM_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

class EEPROMClass {
public:
    uint8_t bytes[4096] = {};
    bool begin(size_t) { return true; }
    uint8_t read(int address) const { return bytes[address]; }
    void write(int address, uint8_t value) { bytes[address] = value; }
    template <typename T> void get(int address, T &value) const {
        memcpy(&value, &bytes[address], sizeof(value));
    }
    template <typename T> void put(int address, const T &value) {
        memcpy(&bytes[address], &value, sizeof(value));
    }
    bool commit() { return true; }
};

extern EEPROMClass EEPROM;

#endif
