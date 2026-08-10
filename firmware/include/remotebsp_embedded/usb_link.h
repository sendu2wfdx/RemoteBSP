#pragma once

#include "remotebsp_embedded/core.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RBSP_USB_FRAME_VERSION 1U
#define RBSP_USB_FRAME_HEADER_SIZE 12U
#define RBSP_USB_LOGICAL_MTU 64U
#define RBSP_USB_MAX_WIRE_FRAME_SIZE \
    (RBSP_USB_FRAME_HEADER_SIZE + RBSP_USB_LOGICAL_MTU)
#define RBSP_USB_DECODER_CAPACITY (RBSP_USB_MAX_WIRE_FRAME_SIZE * 2U)

typedef enum {
    RBSP_USB_DECODE_NEED_MORE = 0,
    RBSP_USB_DECODE_FRAME = 1,
    RBSP_USB_DECODE_INVALID = 2,
} rbsp_usb_decode_result_t;

typedef struct {
    uint16_t size;
    uint8_t data[RBSP_USB_DECODER_CAPACITY];
} rbsp_usb_decoder_t;

size_t rbsp_usb_encode_frame(const rbsp_link_frame_t* frame,
                             uint8_t* output, size_t capacity);
void rbsp_usb_decoder_init(rbsp_usb_decoder_t* decoder);
bool rbsp_usb_decoder_append(rbsp_usb_decoder_t* decoder,
                             const uint8_t* data, size_t size);
rbsp_usb_decode_result_t rbsp_usb_decoder_pop(
    rbsp_usb_decoder_t* decoder, rbsp_link_frame_t* frame);

#ifdef __cplusplus
}
#endif
