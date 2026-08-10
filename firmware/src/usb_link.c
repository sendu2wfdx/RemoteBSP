#include "remotebsp_embedded/usb_link.h"

#include <string.h>

static const uint8_t usb_magic[4] = {'R', 'B', 'U', '1'};

static void put_u16(uint8_t* output, uint16_t value) {
    output[0] = (uint8_t)value;
    output[1] = (uint8_t)(value >> 8U);
}

static void put_u32(uint8_t* output, uint32_t value) {
    for (uint8_t index = 0U; index < 4U; ++index) {
        output[index] = (uint8_t)(value >> (index * 8U));
    }
}

static uint16_t get_u16(const uint8_t* input) {
    return (uint16_t)((uint16_t)input[0] |
                      ((uint16_t)input[1] << 8U));
}

static uint32_t get_u32(const uint8_t* input) {
    uint32_t value = 0U;
    for (uint8_t index = 0U; index < 4U; ++index) {
        value |= (uint32_t)input[index] << (index * 8U);
    }
    return value;
}

size_t rbsp_usb_encode_frame(const rbsp_link_frame_t* frame,
                             uint8_t* output, size_t capacity) {
    if (frame == NULL || output == NULL || frame->length == 0U ||
        frame->length > RBSP_USB_LOGICAL_MTU ||
        capacity < RBSP_USB_FRAME_HEADER_SIZE + frame->length) {
        return 0U;
    }
    memcpy(output, usb_magic, sizeof(usb_magic));
    output[4] = RBSP_USB_FRAME_VERSION;
    output[5] = 0U;
    put_u16(output + 6U, frame->length);
    put_u32(output + 8U, frame->route);
    memcpy(output + RBSP_USB_FRAME_HEADER_SIZE,
           frame->data, frame->length);
    return RBSP_USB_FRAME_HEADER_SIZE + frame->length;
}

void rbsp_usb_decoder_init(rbsp_usb_decoder_t* decoder) {
    if (decoder != NULL) {
        memset(decoder, 0, sizeof(*decoder));
    }
}

bool rbsp_usb_decoder_append(rbsp_usb_decoder_t* decoder,
                             const uint8_t* data, size_t size) {
    if (decoder == NULL || (data == NULL && size != 0U) ||
        size > RBSP_USB_DECODER_CAPACITY - decoder->size) {
        return false;
    }
    if (size != 0U) {
        memcpy(decoder->data + decoder->size, data, size);
        decoder->size = (uint16_t)(decoder->size + size);
    }
    return true;
}

rbsp_usb_decode_result_t rbsp_usb_decoder_pop(
    rbsp_usb_decoder_t* decoder, rbsp_link_frame_t* frame) {
    if (decoder == NULL || frame == NULL) {
        return RBSP_USB_DECODE_INVALID;
    }
    if (decoder->size < RBSP_USB_FRAME_HEADER_SIZE) {
        return RBSP_USB_DECODE_NEED_MORE;
    }
    if (memcmp(decoder->data, usb_magic, sizeof(usb_magic)) != 0 ||
        decoder->data[4] != RBSP_USB_FRAME_VERSION ||
        decoder->data[5] != 0U) {
        decoder->size = 0U;
        return RBSP_USB_DECODE_INVALID;
    }
    const uint16_t payload_size = get_u16(decoder->data + 6U);
    if (payload_size == 0U || payload_size > RBSP_USB_LOGICAL_MTU) {
        decoder->size = 0U;
        return RBSP_USB_DECODE_INVALID;
    }
    const uint16_t total_size =
        (uint16_t)(RBSP_USB_FRAME_HEADER_SIZE + payload_size);
    if (decoder->size < total_size) {
        return RBSP_USB_DECODE_NEED_MORE;
    }
    memset(frame, 0, sizeof(*frame));
    frame->route = get_u32(decoder->data + 8U);
    frame->length = (uint8_t)payload_size;
    memcpy(frame->data,
           decoder->data + RBSP_USB_FRAME_HEADER_SIZE,
           payload_size);
    decoder->size = (uint16_t)(decoder->size - total_size);
    if (decoder->size != 0U) {
        memmove(decoder->data, decoder->data + total_size,
                decoder->size);
    }
    return RBSP_USB_DECODE_FRAME;
}
