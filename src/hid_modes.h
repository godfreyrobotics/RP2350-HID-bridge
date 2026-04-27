#ifndef HID_MODES_H
#define HID_MODES_H

#include <stdint.h>

typedef enum {
    HID_MODE_BRIDGE = 0,
    HID_MODE_RADIO = 1,
    HID_MODE_TELEOP = 2,
    HID_MODE_FULL = 3,
    HID_MODE_COUNT
} hid_mode_t;

const char *hid_mode_name(hid_mode_t mode);
hid_mode_t hid_mode_from_u32(uint32_t value);
void pio_descs_set_mode(hid_mode_t mode);

#endif
