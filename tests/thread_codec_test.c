#include "../src/thread_common/thread_codec.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void round_trip(thread_msg_type_t type, uint16_t sequence)
{
    const thread_packet_t input = {
        .type = type,
        .sequence = sequence,
        .eui64 = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77},
        .rloc16 = 0xabcd,
        .temperature_x100 = -1234,
        .humidity_x100 = 10000,
        .ack_sequence = (uint16_t)(sequence - 1),
    };
    uint8_t wire[THREAD_CODEC_WIRE_SIZE];
    thread_packet_t output;
    assert(thread_codec_encode(&input, wire, sizeof(wire)));
    assert(thread_codec_decode(wire, sizeof(wire), &output));
    assert(output.type == input.type);
    assert(output.sequence == input.sequence);
    assert(memcmp(output.eui64, input.eui64, sizeof(input.eui64)) == 0);
    assert(output.rloc16 == input.rloc16);
    assert(output.temperature_x100 == input.temperature_x100);
    assert(output.humidity_x100 == input.humidity_x100);
    assert(output.ack_sequence == input.ack_sequence);
}

int main(void)
{
    round_trip(THREAD_MSG_ADMIT, 1);
    round_trip(THREAD_MSG_REPORT, UINT16_MAX);
    round_trip(THREAD_MSG_ACK, 0); /* Sequence wrap remains wire-safe. */

    thread_packet_t packet = {.type = THREAD_MSG_REPORT, .sequence = 42};
    uint8_t wire[THREAD_CODEC_WIRE_SIZE];
    thread_packet_t decoded;
    assert(thread_codec_encode(&packet, wire, sizeof(wire)));

    wire[18] ^= 0x01; /* Any payload mutation must fail CRC validation. */
    assert(!thread_codec_decode(wire, sizeof(wire), &decoded));
    wire[18] ^= 0x01;
    assert(thread_codec_decode(wire, sizeof(wire), &decoded));

    assert(!thread_codec_decode(wire, sizeof(wire) - 1, &decoded));
    wire[2] = (uint8_t)(THREAD_CODEC_VERSION + 1);
    assert(!thread_codec_decode(wire, sizeof(wire), &decoded));
    wire[2] = THREAD_CODEC_VERSION;
    wire[3] = 0xff;
    assert(!thread_codec_decode(wire, sizeof(wire), &decoded));

    packet.type = (thread_msg_type_t)0;
    assert(!thread_codec_encode(&packet, wire, sizeof(wire)));
    assert(!thread_codec_encode(NULL, wire, sizeof(wire)));
    assert(!thread_codec_encode(&packet, NULL, sizeof(wire)));

    uint16_t last = 0;
    bool have = false;
    assert(thread_sequence_accept(UINT16_MAX, &last, &have));
    assert(!thread_sequence_accept(UINT16_MAX, &last, &have));
    assert(thread_sequence_accept(0, &last, &have)); /* Wrap is new. */
    assert(!thread_sequence_accept(0, &last, &have));
    assert(!thread_sequence_accept(1, NULL, &have));

    puts("thread codec tests passed");
    return 0;
}
