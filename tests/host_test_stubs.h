#ifndef HID_BRIDGE_HOST_TEST_STUBS_H
#define HID_BRIDGE_HOST_TEST_STUBS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef int64_t absolute_time_t;

absolute_time_t get_absolute_time(void);
int64_t absolute_time_diff_us(absolute_time_t from, absolute_time_t to);
absolute_time_t make_timeout_time_ms(uint32_t delay_ms);
bool time_reached(absolute_time_t target);
uint32_t time_us_32(void);
void sleep_ms(uint32_t delay_ms);
void tight_loop_contents(void);
void stdio_init_all(void);

typedef struct {
    uint32_t scratch[8];
} host_watchdog_hw_t;

extern host_watchdog_hw_t *watchdog_hw;
void watchdog_reboot(uint32_t pc, uint32_t sp, uint32_t delay_ms);

#define FLASH_SECTOR_SIZE 4096u
#define FLASH_PAGE_SIZE 256u
extern uint8_t host_xip_flash[2u * 1024u * 1024u];
#define XIP_BASE ((uintptr_t)host_xip_flash)

void flash_do_cmd(const uint8_t *txbuf, uint8_t *rxbuf, size_t count);
void flash_range_erase(uint32_t offset, size_t count);
void flash_range_program(uint32_t offset, const uint8_t *data, size_t count);
uint32_t save_and_disable_interrupts(void);
void restore_interrupts(uint32_t status);

typedef enum {
    HID_MODE_BRIDGE = 0,
    HID_MODE_RADIO = 1,
    HID_MODE_TELEOP = 2,
    HID_MODE_FULL = 3,
    HID_MODE_COUNT
} hid_mode_t;

const char *hid_mode_name(hid_mode_t mode);
hid_mode_t hid_mode_from_u32(uint32_t value);

typedef struct {
    uint8_t placeholder;
} usb_descriptor_buffers_t;

typedef struct {
    uint8_t pin_dp;
    uint8_t pin_dm;
    uint8_t unused[8];
} pio_usb_configuration_t;

typedef struct {
    uint8_t placeholder;
} usb_device_t;

#define PIO_USB_DEFAULT_CONFIG {0}

usb_device_t *pio_usb_device_init(const pio_usb_configuration_t *config,
                                  const usb_descriptor_buffers_t *buffers);
void pio_usb_device_task(void);

void tusb_init(void);
void tud_task(void);
bool tud_cdc_connected(void);
uint32_t tud_cdc_available(void);
uint32_t tud_cdc_read(void *buffer, uint32_t size);
uint32_t tud_cdc_write_available(void);
uint32_t tud_cdc_write(const void *buffer, uint32_t size);
uint32_t tud_cdc_write_flush(void);

#endif
