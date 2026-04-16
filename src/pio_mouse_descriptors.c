#include <stdint.h>
#include <string.h>

#include "usb_definitions.h"
#include "usb_identity.h"

static const uint8_t pio_device_desc[] = {
    18, DESC_TYPE_DEVICE,
    0x00, 0x02,
    0x00,
    0x00,
    0x00,
    64,
    0xFE, 0xCA,
    0x12, 0x40,
    0x00, 0x01,
    0x01,
    0x02,
    0x03,
    0x01
};

enum {
    REPORT_ID_MOUSE = 0x01,
    REPORT_ID_KEYBOARD = 0x02,
};

static const uint8_t pio_hid_report_desc_composite[] = {
    // Mouse report
    0x05, 0x01,
    0x09, 0x02,
    0xA1, 0x01,
    0x85, REPORT_ID_MOUSE,
    0x09, 0x01,
    0xA1, 0x00,

    0x05, 0x09,
    0x19, 0x01,
    0x29, 0x05,
    0x15, 0x00,
    0x25, 0x01,
    0x95, 0x05,
    0x75, 0x01,
    0x81, 0x02,

    0x95, 0x01,
    0x75, 0x03,
    0x81, 0x01,

    0x05, 0x01,
    0x09, 0x30,
    0x09, 0x31,
    0x09, 0x38,
    0x15, 0x81,
    0x25, 0x7F,
    0x75, 0x08,
    0x95, 0x03,
    0x81, 0x06,

    0x05, 0x0C,
    0x0A, 0x38, 0x02,
    0x15, 0x81,
    0x25, 0x7F,
    0x75, 0x08,
    0x95, 0x01,
    0x81, 0x06,

    0xC0,
    0xC0,

    // Boot keyboard report
    0x05, 0x01,
    0x09, 0x06,
    0xA1, 0x01,
    0x85, REPORT_ID_KEYBOARD,

    0x05, 0x07,
    0x19, 0xE0,
    0x29, 0xE7,
    0x15, 0x00,
    0x25, 0x01,
    0x75, 0x01,
    0x95, 0x08,
    0x81, 0x02,

    0x75, 0x08,
    0x95, 0x01,
    0x81, 0x01,

    0x75, 0x08,
    0x95, 0x06,
    0x15, 0x00,
    0x25, 0x65,
    0x05, 0x07,
    0x19, 0x00,
    0x29, 0x65,
    0x81, 0x00,

    0xC0
};

static const uint8_t *pio_hid_reports[] = {
    pio_hid_report_desc_composite
};

static const uint8_t pio_config_desc[] = {
    9,  DESC_TYPE_CONFIG,
    34, 0,
    1,
    1,
    0,
    0xA0,
    50,

    9,  DESC_TYPE_INTERFACE,
    0,
    0,
    1,
    CLASS_HID,
    0x01,
    0x01,
    0,

    9,  DESC_TYPE_HID,
    0x11, 0x01,
    0x00,
    0x01,
    DESC_TYPE_HID_REPORT,
    sizeof(pio_hid_report_desc_composite) & 0xFF,
    (sizeof(pio_hid_report_desc_composite) >> 8) & 0xFF,

    7,  DESC_TYPE_ENDPOINT,
    0x81,
    EP_ATTR_INTERRUPT,
    8, 0,
    10
};

static string_descriptor_t pio_string_desc[4];
usb_descriptor_buffers_t pio_descs;

static void fill_string_descriptor(string_descriptor_t *dst, const char *ascii) {
    memset(dst, 0, sizeof(*dst));

    size_t n = strlen(ascii);
    if (n > 31) n = 31;

    dst->length = (uint8_t)(2 + 2 * n);
    dst->type = DESC_TYPE_STRING;

    for (size_t i = 0; i < n; i++) {
        dst->string[2 * i] = (uint8_t)ascii[i];
        dst->string[2 * i + 1] = 0;
    }
}

void pio_descs_init(void) {
    memset(pio_string_desc, 0, sizeof(pio_string_desc));

    pio_string_desc[0].length = 4;
    pio_string_desc[0].type = DESC_TYPE_STRING;
    pio_string_desc[0].string[0] = 0x09;
    pio_string_desc[0].string[1] = 0x04;

    fill_string_descriptor(&pio_string_desc[1], USB_MANUFACTURER_STRING);
    fill_string_descriptor(&pio_string_desc[2], USB_POINTER_PRODUCT_STRING);
    fill_string_descriptor(&pio_string_desc[3], USB_SHARED_SERIAL_STRING);

    pio_descs.device = pio_device_desc;
    pio_descs.config = pio_config_desc;
    pio_descs.hid_report = pio_hid_reports;
    pio_descs.string = pio_string_desc;
}
