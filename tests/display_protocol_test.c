#include "../src/common/display_protocol.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    unsigned calls;
    display_frame_t last;
} capture_t;

static void capture_frame(const display_frame_t *frame, void *context) {
    capture_t *capture = (capture_t *)context;
    capture->calls++;
    capture->last = *frame;
}

int main(void) {
    assert(display_protocol_self_test());

    uint8_t payload[DISPLAY_PROTO_MAX_PAYLOAD];
    memset(payload, 0x5A, sizeof(payload));
    uint8_t encoded[DISPLAY_PROTO_FRAME_MAX];
    size_t size = display_encode_frame(DISPLAY_MSG_SNAPSHOT, 0, 65535, payload,
                                       sizeof(payload), encoded, sizeof(encoded));
    assert(size == sizeof(encoded));
    assert(display_encode_frame(DISPLAY_MSG_SNAPSHOT, 0, 1, payload,
                                DISPLAY_PROTO_MAX_PAYLOAD + 1, encoded, sizeof(encoded)) == 0);

    display_parser_t parser;
    capture_t capture = {0};
    display_parser_init(&parser);
    for (size_t i = 0; i < size; ++i) {
        display_parser_push(&parser, &encoded[i], 1, capture_frame, &capture);
    }
    assert(capture.calls == 1);
    assert(capture.last.sequence == 65535);
    assert(capture.last.length == DISPLAY_PROTO_MAX_PAYLOAD);
    assert(memcmp(capture.last.payload, payload, sizeof(payload)) == 0);

    display_wifi_credentials_t credentials = {1, "factory-network", "secret"};
    size = display_encode_frame(DISPLAY_MSG_WIFI_CREDENTIALS, 0, 42,
                                &credentials, sizeof(credentials),
                                encoded, sizeof(encoded));
    assert(size == DISPLAY_PROTO_HEADER_SIZE + sizeof(credentials) + 2u);
    capture.calls = 0;
    display_parser_init(&parser);
    display_parser_push(&parser, encoded, size, capture_frame, &capture);
    assert(capture.calls == 1);
    assert(capture.last.type == DISPLAY_MSG_WIFI_CREDENTIALS);
    assert(capture.last.length == sizeof(credentials));
    assert(memcmp(capture.last.payload, &credentials, sizeof(credentials)) == 0);

    puts("display_protocol: all tests passed");
    return 0;
}
