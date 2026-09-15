#include "thread_codec.h"

#include <string.h>

#define MAGIC_0 0x53u
#define MAGIC_1 0x54u

static uint16_t get_u16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static void put_u16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
}

static uint32_t crc32(const uint8_t *data, size_t length)
{
    uint32_t crc = UINT32_C(0xffffffff);
    for (size_t i = 0; i < length; ++i) {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (UINT32_C(0xedb88320) & (uint32_t)-(int32_t)(crc & 1u));
        }
    }
    return ~crc;
}

bool thread_codec_encode(const thread_packet_t *packet, uint8_t *wire, size_t wire_size)
{
    if (!packet || !wire || wire_size < THREAD_CODEC_WIRE_SIZE ||
        packet->type < THREAD_MSG_ADMIT || packet->type > THREAD_MSG_ACK) {
        return false;
    }

    memset(wire, 0, THREAD_CODEC_WIRE_SIZE);
    wire[0] = MAGIC_0;
    wire[1] = MAGIC_1;
    wire[2] = THREAD_CODEC_VERSION;
    wire[3] = (uint8_t)packet->type;
    put_u16(&wire[4], packet->sequence);
    memcpy(&wire[6], packet->eui64, THREAD_CODEC_EUI64_SIZE);
    put_u16(&wire[14], packet->rloc16);
    put_u16(&wire[16], (uint16_t)packet->temperature_x100);
    put_u16(&wire[18], packet->humidity_x100);
    put_u16(&wire[20], packet->ack_sequence);
    const uint32_t crc = crc32(wire, 24);
    wire[24] = (uint8_t)crc;
    wire[25] = (uint8_t)(crc >> 8);
    wire[26] = (uint8_t)(crc >> 16);
    wire[27] = (uint8_t)(crc >> 24);
    return true;
}

bool thread_codec_decode(const uint8_t *wire, size_t wire_size, thread_packet_t *packet)
{
    if (!wire || !packet || wire_size != THREAD_CODEC_WIRE_SIZE ||
        wire[0] != MAGIC_0 || wire[1] != MAGIC_1 ||
        wire[2] != THREAD_CODEC_VERSION ||
        wire[3] < THREAD_MSG_ADMIT || wire[3] > THREAD_MSG_ACK) {
        return false;
    }

    const uint32_t expected = (uint32_t)wire[24] |
                              ((uint32_t)wire[25] << 8) |
                              ((uint32_t)wire[26] << 16) |
                              ((uint32_t)wire[27] << 24);
    if (crc32(wire, 24) != expected) {
        return false;
    }

    memset(packet, 0, sizeof(*packet));
    packet->type = (thread_msg_type_t)wire[3];
    packet->sequence = get_u16(&wire[4]);
    memcpy(packet->eui64, &wire[6], THREAD_CODEC_EUI64_SIZE);
    packet->rloc16 = get_u16(&wire[14]);
    packet->temperature_x100 = (int16_t)get_u16(&wire[16]);
    packet->humidity_x100 = get_u16(&wire[18]);
    packet->ack_sequence = get_u16(&wire[20]);
    return true;
}

bool thread_sequence_accept(uint16_t sequence, uint16_t *last_sequence,
                            bool *have_sequence)
{
    if (!last_sequence || !have_sequence) {
        return false;
    }
    if (*have_sequence && *last_sequence == sequence) {
        return false;
    }
    *last_sequence = sequence;
    *have_sequence = true;
    return true;
}
