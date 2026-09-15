#ifndef SHUNYA_THREAD_CODEC_H
#define SHUNYA_THREAD_CODEC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define THREAD_CODEC_VERSION       1u
#define THREAD_CODEC_WIRE_SIZE     28u
#define THREAD_CODEC_EUI64_SIZE    8u

typedef enum {
    THREAD_MSG_ADMIT = 1,
    THREAD_MSG_REPORT = 2,
    THREAD_MSG_ACK = 3,
} thread_msg_type_t;

typedef struct {
    thread_msg_type_t type;
    uint16_t sequence;
    uint8_t eui64[THREAD_CODEC_EUI64_SIZE];
    uint16_t rloc16;
    int16_t temperature_x100;
    uint16_t humidity_x100;
    uint16_t ack_sequence;
} thread_packet_t;

/* Fixed-width, endian-defined and dependency-free so desktop tests can compile it. */
bool thread_codec_encode(const thread_packet_t *packet, uint8_t *wire, size_t wire_size);
bool thread_codec_decode(const uint8_t *wire, size_t wire_size, thread_packet_t *packet);
/* Accepts the first/new sequence and rejects an immediate UDP retry. */
bool thread_sequence_accept(uint16_t sequence, uint16_t *last_sequence,
                            bool *have_sequence);

#ifdef __cplusplus
}
#endif

#endif
