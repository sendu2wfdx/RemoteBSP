#include "remotebsp_embedded/usb_link.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(condition)                                                       \
    do {                                                                       \
        if (!(condition)) {                                                    \
            fprintf(stderr, "%s:%d 检查失败: %s\n", __FILE__, __LINE__,      \
                    #condition);                                               \
            ++failures;                                                        \
        }                                                                      \
    } while (0)

static void test_split_and_coalesced(void) {
    rbsp_link_frame_t first = {.identifier = 0x700U, .length = 3U,
                               .data = {1U, 2U, 3U}};
    rbsp_link_frame_t second = {.identifier = 0x581U, .length = 64U};
    memset(second.data, 0x5A, sizeof(second.data));
    uint8_t first_wire[RBSP_USB_MAX_WIRE_FRAME_SIZE];
    uint8_t second_wire[RBSP_USB_MAX_WIRE_FRAME_SIZE];
    const size_t first_size = rbsp_usb_encode_frame(
        &first, first_wire, sizeof(first_wire));
    const size_t second_size = rbsp_usb_encode_frame(
        &second, second_wire, sizeof(second_wire));
    CHECK(first_size == RBSP_USB_FRAME_HEADER_SIZE + 3U);
    CHECK(second_size == RBSP_USB_MAX_WIRE_FRAME_SIZE);

    rbsp_usb_decoder_t decoder;
    rbsp_usb_decoder_init(&decoder);
    CHECK(rbsp_usb_decoder_append(&decoder, first_wire, 5U));
    rbsp_link_frame_t output;
    CHECK(rbsp_usb_decoder_pop(&decoder, &output) ==
          RBSP_USB_DECODE_NEED_MORE);
    CHECK(rbsp_usb_decoder_append(&decoder, first_wire + 5U,
                                  first_size - 5U));
    CHECK(rbsp_usb_decoder_append(&decoder, second_wire, second_size));
    CHECK(rbsp_usb_decoder_pop(&decoder, &output) ==
          RBSP_USB_DECODE_FRAME);
    CHECK(output.identifier == first.identifier);
    CHECK(output.length == first.length);
    CHECK(memcmp(output.data, first.data, first.length) == 0);
    CHECK(rbsp_usb_decoder_pop(&decoder, &output) ==
          RBSP_USB_DECODE_FRAME);
    CHECK(output.identifier == second.identifier);
    CHECK(output.length == second.length);
    CHECK(memcmp(output.data, second.data, second.length) == 0);
}

static void test_invalid(void) {
    rbsp_link_frame_t frame = {.identifier = 1U, .length = 1U,
                               .data = {9U}};
    uint8_t wire[RBSP_USB_MAX_WIRE_FRAME_SIZE];
    const size_t size = rbsp_usb_encode_frame(&frame, wire, sizeof(wire));
    wire[0] = 0U;
    rbsp_usb_decoder_t decoder;
    rbsp_usb_decoder_init(&decoder);
    CHECK(rbsp_usb_decoder_append(&decoder, wire, size));
    CHECK(rbsp_usb_decoder_pop(&decoder, &frame) ==
          RBSP_USB_DECODE_INVALID);
    CHECK(decoder.size == 0U);
}

int main(void) {
    test_split_and_coalesced();
    test_invalid();
    if (failures != 0) {
        fprintf(stderr, "%d 个固件 USB 链路测试失败\n", failures);
        return 1;
    }
    puts("固件 USB 链路编解码测试通过");
    return 0;
}
