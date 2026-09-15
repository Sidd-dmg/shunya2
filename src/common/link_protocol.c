#include "link_protocol.h"

/* Header-only helpers live in link_protocol.h. This unit exists so both
 * Arduino and ESP-IDF src filters always compile at least one common file. */
uint8_t link_protocol_version(void) {
    return 1;
}
