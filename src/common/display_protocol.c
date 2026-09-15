#include "display_protocol.h"
#include <string.h>

static uint16_t read_u16_le(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static void write_u16_le(uint8_t *p, uint16_t value) {
    p[0] = (uint8_t)(value & 0xFFu);
    p[1] = (uint8_t)(value >> 8);
}

uint16_t display_crc16(const uint8_t *data, size_t length) {
    uint16_t crc = 0xFFFFu;
    for (size_t i = 0; i < length; ++i) {
        crc ^= (uint16_t)data[i] << 8;
        for (uint8_t bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u) : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

size_t display_encode_frame(uint8_t type, uint8_t flags, uint16_t sequence,
                            const void *payload, uint16_t length,
                            uint8_t *output, size_t output_size) {
    const size_t total = DISPLAY_PROTO_HEADER_SIZE + (size_t)length + 2u;
    if (!output || length > DISPLAY_PROTO_MAX_PAYLOAD || output_size < total ||
        (length && !payload)) {
        return 0;
    }
    output[0] = DISPLAY_PROTO_MAGIC_0;
    output[1] = DISPLAY_PROTO_MAGIC_1;
    output[2] = DISPLAY_PROTO_VERSION;
    output[3] = type;
    output[4] = flags;
    output[5] = 0;
    write_u16_le(&output[6], sequence);
    write_u16_le(&output[8], length);
    if (length) {
        memcpy(&output[DISPLAY_PROTO_HEADER_SIZE], payload, length);
    }
    write_u16_le(&output[DISPLAY_PROTO_HEADER_SIZE + length],
                 display_crc16(&output[2], 8u + length));
    return total;
}

void display_parser_init(display_parser_t *parser) {
    if (parser) {
        memset(parser, 0, sizeof(*parser));
    }
}

static void discard_prefix(display_parser_t *parser, uint16_t count) {
    if (count >= parser->used) {
        parser->used = 0;
        return;
    }
    memmove(parser->data, parser->data + count, parser->used - count);
    parser->used = (uint16_t)(parser->used - count);
}

static void parse_available(display_parser_t *parser, display_frame_callback_t callback, void *context) {
    while (parser->used >= 2) {
        uint16_t start = 0;
        while (start + 1u < parser->used &&
               (parser->data[start] != DISPLAY_PROTO_MAGIC_0 ||
                parser->data[start + 1u] != DISPLAY_PROTO_MAGIC_1)) {
            ++start;
        }
        if (start) {
            parser->framing_errors += start;
            discard_prefix(parser, start);
        }
        if (parser->used < DISPLAY_PROTO_HEADER_SIZE) {
            return;
        }
        if (parser->data[2] != DISPLAY_PROTO_VERSION) {
            ++parser->framing_errors;
            discard_prefix(parser, 1);
            continue;
        }
        const uint16_t payload_length = read_u16_le(&parser->data[8]);
        if (payload_length > DISPLAY_PROTO_MAX_PAYLOAD) {
            ++parser->framing_errors;
            discard_prefix(parser, 1);
            continue;
        }
        const uint16_t total = (uint16_t)(DISPLAY_PROTO_HEADER_SIZE + payload_length + 2u);
        if (parser->used < total) {
            return;
        }
        const uint16_t expected = read_u16_le(&parser->data[DISPLAY_PROTO_HEADER_SIZE + payload_length]);
        const uint16_t actual = display_crc16(&parser->data[2], 8u + payload_length);
        if (actual != expected) {
            ++parser->crc_errors;
            discard_prefix(parser, 1);
            continue;
        }
        if (callback) {
            display_frame_t frame;
            frame.type = parser->data[3];
            frame.flags = parser->data[4];
            frame.sequence = read_u16_le(&parser->data[6]);
            frame.length = payload_length;
            if (payload_length) {
                memcpy(frame.payload, &parser->data[DISPLAY_PROTO_HEADER_SIZE], payload_length);
            }
            callback(&frame, context);
        }
        discard_prefix(parser, total);
    }
}

void display_parser_push(display_parser_t *parser, const uint8_t *data, size_t length,
                         display_frame_callback_t callback, void *context) {
    if (!parser || (!data && length)) {
        return;
    }
    while (length) {
        if (parser->used == DISPLAY_PROTO_FRAME_MAX) {
            ++parser->framing_errors;
            discard_prefix(parser, 1);
        }
        size_t room = DISPLAY_PROTO_FRAME_MAX - parser->used;
        size_t copy = length < room ? length : room;
        memcpy(parser->data + parser->used, data, copy);
        parser->used = (uint16_t)(parser->used + copy);
        data += copy;
        length -= copy;
        parse_available(parser, callback, context);
    }
}

typedef struct {
    uint8_t calls;
    display_frame_t frame;
} self_test_capture_t;

static void self_test_callback(const display_frame_t *frame, void *context) {
    self_test_capture_t *capture = (self_test_capture_t *)context;
    ++capture->calls;
    capture->frame = *frame;
}

bool display_protocol_self_test(void) {
    static const uint8_t payload[] = {0x00, 0xA5, 0x5A, 0xFF, 0x11};
    uint8_t encoded[DISPLAY_PROTO_FRAME_MAX];
    size_t size = display_encode_frame(DISPLAY_MSG_ECHO, 0x03, 0x1234, payload,
                                       sizeof(payload), encoded, sizeof(encoded));
    if (size != DISPLAY_PROTO_HEADER_SIZE + sizeof(payload) + 2u) return false;

    display_parser_t parser;
    self_test_capture_t capture;
    memset(&capture, 0, sizeof(capture));
    display_parser_init(&parser);
    display_parser_push(&parser, encoded, 3, self_test_callback, &capture);
    if (capture.calls != 0) return false;
    display_parser_push(&parser, encoded + 3, size - 3, self_test_callback, &capture);
    if (capture.calls != 1 || capture.frame.type != DISPLAY_MSG_ECHO ||
        capture.frame.sequence != 0x1234 || capture.frame.length != sizeof(payload) ||
        memcmp(capture.frame.payload, payload, sizeof(payload)) != 0) return false;

    memset(&capture, 0, sizeof(capture));
    display_parser_init(&parser);
    encoded[DISPLAY_PROTO_HEADER_SIZE] ^= 0x01;
    display_parser_push(&parser, encoded, size, self_test_callback, &capture);
    if (capture.calls != 0 || parser.crc_errors != 1) return false;
    encoded[DISPLAY_PROTO_HEADER_SIZE] ^= 0x01;

    uint8_t noisy[DISPLAY_PROTO_FRAME_MAX + 3];
    noisy[0] = 0x12; noisy[1] = 0xA5; noisy[2] = 0x00;
    memcpy(noisy + 3, encoded, size);
    memset(&capture, 0, sizeof(capture));
    display_parser_init(&parser);
    display_parser_push(&parser, noisy, size + 3, self_test_callback, &capture);
    return capture.calls == 1 && parser.framing_errors >= 3;
}
