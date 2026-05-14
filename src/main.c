#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

#include "pico/stdlib.h"
#include "hardware/watchdog.h"
#include "hardware/structs/watchdog.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "hardware/regs/addressmap.h"
#include "tusb.h"

#include "pio_usb.h"
#include "pio_usb_configuration.h"
#include "usb_definitions.h"
#include "hid_modes.h"

extern usb_descriptor_buffers_t pio_descs;
void pio_descs_init(void);
void pio_descs_set_mode(hid_mode_t mode);
bool pio_usb_device_transfer(uint8_t ep_address, uint8_t *buffer, uint16_t buflen);

#define CDC_LINE_BUF_SIZE 384
#define HEARTBEAT_TIMEOUT_MS 2000
#define ACTION_QUEUE_SIZE 512

#define RADIO_AXIS_COUNT 8
#define TELEOP_AXES_PER_BANK 30
#define TELEOP_BANK_COUNT 2
#define TELEOP_AXIS_COUNT (TELEOP_AXES_PER_BANK * TELEOP_BANK_COUNT)

#define MODE_SCRATCH_MAGIC_INDEX 0
#define MODE_SCRATCH_VALUE_INDEX 1
#define MODE_SCRATCH_MAGIC 0x4849444Du

#define BOARD_SCRATCH_MAGIC_INDEX 2
#define BOARD_SCRATCH_VALUE_INDEX 3
#define BOARD_SCRATCH_MAGIC 0x424F4152u

#define BOARD_CONFIG_MAGIC 0x42434647u
#define BOARD_CONFIG_VERSION 1u
#define BOARD_CONFIG_FLASH_OFFSET ((2u * 1024u * 1024u) - FLASH_SECTOR_SIZE)
#define BOARD_AUTO_FLASH_THRESHOLD_BYTES (8u * 1024u * 1024u)
#define BOARD_USBA_PIO_STARTUP_DELAY_MS 500

typedef enum {
    BOARD_WAVESHARE_RP2350_PIZERO = 0,
    BOARD_WAVESHARE_RP2350_USB_A = 1,
} board_type_t;

typedef enum {
    BOARD_SELECT_SOURCE_AUTO = 0,
    BOARD_SELECT_SOURCE_MANUAL = 1,
} board_select_source_t;

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t source;
    uint32_t board;
    uint32_t checksum;
    uint32_t reserved[3];
} persistent_board_config_t;

typedef struct {
    const char *command_name;
    const char *display_name;
    uint8_t pio_usb_dp_pin;
    uint8_t pio_usb_dm_pin;
    uint16_t pio_startup_delay_ms;
} board_profile_t;

static const board_profile_t BOARD_PROFILES[] = {
    [BOARD_WAVESHARE_RP2350_PIZERO] = {
        .command_name = "PIZERO",
        .display_name = "waveshare_rp2350_pizero",
        .pio_usb_dp_pin = 28,
        .pio_usb_dm_pin = 29,
        .pio_startup_delay_ms = 0,
    },
    [BOARD_WAVESHARE_RP2350_USB_A] = {
        .command_name = "USBA",
        .display_name = "waveshare_rp2350_usb_a",
        .pio_usb_dp_pin = 12,
        .pio_usb_dm_pin = 13,
        .pio_startup_delay_ms = BOARD_USBA_PIO_STARTUP_DELAY_MS,
    },
};

#define BOARD_PROFILE_COUNT (sizeof(BOARD_PROFILES) / sizeof(BOARD_PROFILES[0]))

enum {
    REPORT_ID_MOUSE = 0x01,
    REPORT_ID_KEYBOARD = 0x02,
    REPORT_ID_RADIO = 0x03,
    REPORT_ID_TELEOP_BANK = 0x04,
};

typedef struct {
    uint8_t buttons;
    int8_t x;
    int8_t y;
    int8_t wheel;
    int8_t pan;
    bool dirty;
} mouse_state_t;

typedef struct {
    uint8_t modifiers;
    uint8_t keycodes[6];
    bool dirty;
} keyboard_state_t;

typedef struct {
    int16_t axes[RADIO_AXIS_COUNT];
    uint16_t buttons;
    bool dirty;
} radio_state_t;

typedef struct {
    int16_t axes[TELEOP_AXIS_COUNT];
    bool bank_dirty[TELEOP_BANK_COUNT];
    uint8_t sequence;
    uint8_t bank_sequence[TELEOP_BANK_COUNT];
    uint8_t next_bank_to_try;
} teleop_state_t;

typedef enum {
    ACT_NONE = 0,
    ACT_SET_BUTTONS,
    ACT_DELAY_ONLY,
    ACT_REPORT_REL,
    ACT_RESET_ALL
} action_type_t;

typedef struct {
    action_type_t type;
    uint32_t delay_ms;
    uint8_t buttons;
    int8_t dx;
    int8_t dy;
    int8_t wheel;
    int8_t pan;
} action_t;

typedef struct {
    float x;
    float y;
} vec2f_t;

static mouse_state_t g_mouse = {0};
static keyboard_state_t g_kbd = {0};
static radio_state_t g_radio = {0};
static teleop_state_t g_teleop = {0};
static hid_mode_t g_hid_mode = HID_MODE_BRIDGE;
static board_type_t g_board_type = BOARD_WAVESHARE_RP2350_PIZERO;
static board_type_t g_auto_detected_board_type = BOARD_WAVESHARE_RP2350_PIZERO;
static board_select_source_t g_board_select_source = BOARD_SELECT_SOURCE_AUTO;
static uint32_t g_detected_flash_size_bytes = 0;
static char g_line_buf[CDC_LINE_BUF_SIZE];
static size_t g_line_len = 0;
static absolute_time_t g_last_heartbeat;
static absolute_time_t g_last_status;
static bool g_watchdog_tripped = false;

static action_t g_queue[ACTION_QUEUE_SIZE];
static uint16_t g_q_head = 0;
static uint16_t g_q_tail = 0;
static uint32_t g_queue_deadline_ms = 0;
static bool g_motion_owns_button = false;
static uint8_t g_motion_owned_mask = 0;

static void mouse_mark_button_state(uint8_t buttons);
static void cdc_write_line(const char *s);

static inline int8_t clamp_i8(int v) {
    if (v < -127) return -127;
    if (v > 127) return 127;
    return (int8_t)v;
}

static inline int clamp_cdc_axis(int v) {
    if (v < -1000) return -1000;
    if (v > 1000) return 1000;
    return v;
}

static inline int16_t cdc_axis_to_hid_i16(int v) {
    v = clamp_cdc_axis(v);
    return (int16_t)((v * 32767) / 1000);
}

static bool mode_supports_bridge(void) {
    return g_hid_mode == HID_MODE_BRIDGE || g_hid_mode == HID_MODE_FULL;
}

static bool mode_supports_radio(void) {
    return g_hid_mode == HID_MODE_RADIO || g_hid_mode == HID_MODE_FULL;
}

static bool mode_supports_teleop(void) {
    return g_hid_mode == HID_MODE_TELEOP || g_hid_mode == HID_MODE_FULL;
}

static hid_mode_t read_requested_mode(void) {
    if (watchdog_hw->scratch[MODE_SCRATCH_MAGIC_INDEX] == MODE_SCRATCH_MAGIC) {
        return hid_mode_from_u32(watchdog_hw->scratch[MODE_SCRATCH_VALUE_INDEX]);
    }
    return HID_MODE_BRIDGE;
}

static void mode_store_and_reboot(hid_mode_t mode) {
    watchdog_hw->scratch[MODE_SCRATCH_MAGIC_INDEX] = MODE_SCRATCH_MAGIC;
    watchdog_hw->scratch[MODE_SCRATCH_VALUE_INDEX] = (uint32_t)mode;
    sleep_ms(50);
    watchdog_reboot(0, 0, 10);
    while (true) {
        tight_loop_contents();
    }
}

static bool board_type_is_valid(uint32_t value) {
    return value < BOARD_PROFILE_COUNT;
}

static const char *board_select_source_name(board_select_source_t source) {
    switch (source) {
        case BOARD_SELECT_SOURCE_AUTO:
            return "auto";
        case BOARD_SELECT_SOURCE_MANUAL:
            return "manual";
        default:
            return "unknown";
    }
}

static uint32_t board_config_checksum(const persistent_board_config_t *config) {
    return config->magic ^
           config->version ^
           config->source ^
           config->board ^
           0xA5A55A5Au;
}

static bool board_config_is_valid(const persistent_board_config_t *config) {
    if (config->magic != BOARD_CONFIG_MAGIC) return false;
    if (config->version != BOARD_CONFIG_VERSION) return false;
    if (config->checksum != board_config_checksum(config)) return false;

    if (config->source != BOARD_SELECT_SOURCE_AUTO &&
        config->source != BOARD_SELECT_SOURCE_MANUAL) {
        return false;
    }

    if (config->source == BOARD_SELECT_SOURCE_MANUAL &&
        !board_type_is_valid(config->board)) {
        return false;
    }

    return true;
}

static bool board_read_persistent_config(persistent_board_config_t *config_out) {
    const persistent_board_config_t *flash_config =
        (const persistent_board_config_t *)(XIP_BASE + BOARD_CONFIG_FLASH_OFFSET);

    memcpy(config_out, flash_config, sizeof(*config_out));
    return board_config_is_valid(config_out);
}

static uint32_t board_detect_flash_size_bytes(void) {
    uint8_t txbuf[4] = {0x9F, 0, 0, 0};
    uint8_t rxbuf[4] = {0, 0, 0, 0};

    flash_do_cmd(txbuf, rxbuf, sizeof(txbuf));

    uint8_t capacity_code = rxbuf[3];
    if (capacity_code < 20 || capacity_code > 30) {
        return 0;
    }

    return (uint32_t)(1u << capacity_code);
}

static board_type_t board_auto_detect_from_flash_size(uint32_t flash_size_bytes) {
    if (flash_size_bytes >= BOARD_AUTO_FLASH_THRESHOLD_BYTES) {
        return BOARD_WAVESHARE_RP2350_PIZERO;
    }

    if (flash_size_bytes > 0) {
        return BOARD_WAVESHARE_RP2350_USB_A;
    }

    return BOARD_WAVESHARE_RP2350_PIZERO;
}

static void board_write_persistent_config(const persistent_board_config_t *config) {
    uint8_t page[FLASH_PAGE_SIZE];
    memset(page, 0xFF, sizeof(page));

    if (config != NULL) {
        memcpy(page, config, sizeof(*config));
    }

    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(BOARD_CONFIG_FLASH_OFFSET, FLASH_SECTOR_SIZE);
    if (config != NULL) {
        flash_range_program(BOARD_CONFIG_FLASH_OFFSET, page, sizeof(page));
    }
    restore_interrupts(ints);
}

static void board_save_manual_override(board_type_t board) {
    persistent_board_config_t config = {
        .magic = BOARD_CONFIG_MAGIC,
        .version = BOARD_CONFIG_VERSION,
        .source = BOARD_SELECT_SOURCE_MANUAL,
        .board = (uint32_t)board,
        .checksum = 0,
        .reserved = {0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu},
    };
    config.checksum = board_config_checksum(&config);
    board_write_persistent_config(&config);
}

static void board_clear_persistent_override(void) {
    board_write_persistent_config(NULL);
}

static board_type_t read_requested_board(void) {
    g_detected_flash_size_bytes = board_detect_flash_size_bytes();
    g_auto_detected_board_type = board_auto_detect_from_flash_size(g_detected_flash_size_bytes);

    persistent_board_config_t config;
    if (board_read_persistent_config(&config)) {
        if (config.source == BOARD_SELECT_SOURCE_MANUAL) {
            g_board_select_source = BOARD_SELECT_SOURCE_MANUAL;
            return (board_type_t)config.board;
        }
    }

    g_board_select_source = BOARD_SELECT_SOURCE_AUTO;
    return g_auto_detected_board_type;
}

static const board_profile_t *current_board_profile(void) {
    return &BOARD_PROFILES[g_board_type];
}

static bool parse_board_name(const char *s, board_type_t *board_out) {
    if (strcmp(s, "PIZERO") == 0 ||
        strcmp(s, "PI_ZERO") == 0 ||
        strcmp(s, "RP2350_PIZERO") == 0 ||
        strcmp(s, "WAVESHARE_RP2350_PIZERO") == 0) {
        *board_out = BOARD_WAVESHARE_RP2350_PIZERO;
        return true;
    }

    if (strcmp(s, "USBA") == 0 ||
        strcmp(s, "USB_A") == 0 ||
        strcmp(s, "RP2350_USB_A") == 0 ||
        strcmp(s, "WAVESHARE_RP2350_USB_A") == 0) {
        *board_out = BOARD_WAVESHARE_RP2350_USB_A;
        return true;
    }

    return false;
}

static bool parse_board_auto_name(const char *s) {
    return strcmp(s, "AUTO") == 0 ||
           strcmp(s, "AUTODETECT") == 0 ||
           strcmp(s, "AUTO_DETECT") == 0;
}

static void board_reboot(void) {
    sleep_ms(50);
    watchdog_reboot(0, 0, 10);
    while (true) {
        tight_loop_contents();
    }
}

static void board_store_and_reboot(board_type_t board) {
    board_save_manual_override(board);
    board_reboot();
}

static void board_clear_and_reboot(void) {
    board_clear_persistent_override();
    board_reboot();
}

static void send_board_status(void) {
    const board_profile_t *profile = current_board_profile();
    const board_profile_t *auto_profile = &BOARD_PROFILES[g_auto_detected_board_type];

    char msg[192];
    snprintf(
        msg,
        sizeof(msg),
        "BOARD %s dp=%u dm=%u source=%s auto=%s flash=%lu",
        profile->display_name,
        profile->pio_usb_dp_pin,
        profile->pio_usb_dm_pin,
        board_select_source_name(g_board_select_source),
        auto_profile->display_name,
        (unsigned long)g_detected_flash_size_bytes
    );
    cdc_write_line(msg);
}

static inline uint32_t now_ms(void) {
    return to_ms_since_boot(get_absolute_time());
}

static int rand_range(int min_v, int max_v) {
    if (max_v < min_v) {
        int t = min_v;
        min_v = max_v;
        max_v = t;
    }
    uint32_t span = (uint32_t)(max_v - min_v + 1);
    return min_v + (int)(rand() % span);
}

static float rand_float(float a, float b) {
    float r = (float)rand() / (float)RAND_MAX;
    return a + (b - a) * r;
}

static void cdc_write_line(const char *s) {
    if (tud_cdc_connected()) {
        tud_cdc_write_str(s);
        tud_cdc_write_str("\r\n");
        tud_cdc_write_flush();
    }
}

static bool queue_empty(void) {
    return g_q_head == g_q_tail;
}

static bool queue_full(void) {
    return ((g_q_tail + 1) % ACTION_QUEUE_SIZE) == g_q_head;
}

static bool queue_push(action_t a) {
    if (queue_full()) return false;
    g_queue[g_q_tail] = a;
    g_q_tail = (g_q_tail + 1) % ACTION_QUEUE_SIZE;
    return true;
}

static bool queue_peek(action_t *out) {
    if (queue_empty()) return false;
    *out = g_queue[g_q_head];
    return true;
}

static void queue_pop(void) {
    if (!queue_empty()) {
        g_q_head = (g_q_head + 1) % ACTION_QUEUE_SIZE;
    }
}

static void queue_clear(void) {
    g_q_head = 0;
    g_q_tail = 0;
    g_queue_deadline_ms = 0;
}

static void release_motion_owned_button_if_needed(void) {
    if (g_motion_owns_button && (g_mouse.buttons & g_motion_owned_mask)) {
        mouse_mark_button_state((uint8_t)(g_mouse.buttons & (uint8_t)~g_motion_owned_mask));
    }
    g_motion_owns_button = false;
    g_motion_owned_mask = 0;
}

static void preempt_motion_plan(bool release_owned_button) {
    queue_clear();
    if (release_owned_button) {
        release_motion_owned_button_if_needed();
    } else {
        g_motion_owns_button = false;
        g_motion_owned_mask = 0;
    }
}

static void mouse_mark_report(uint8_t buttons, int dx, int dy, int wheel, int pan) {
    g_mouse.buttons = buttons & 0x1F;
    g_mouse.x = clamp_i8(dx);
    g_mouse.y = clamp_i8(dy);
    g_mouse.wheel = clamp_i8(wheel);
    g_mouse.pan = clamp_i8(pan);
    g_mouse.dirty = true;
}

static void mouse_mark_button_state(uint8_t buttons) {
    mouse_mark_report(buttons, 0, 0, 0, 0);
}

static void mouse_release_all(void) {
    g_mouse.buttons = 0;
    g_mouse.x = 0;
    g_mouse.y = 0;
    g_mouse.wheel = 0;
    g_mouse.pan = 0;
    g_mouse.dirty = true;
    queue_clear();
    g_motion_owns_button = false;
    g_motion_owned_mask = 0;
}

static void keyboard_mark_dirty(void) {
    g_kbd.dirty = true;
}

static void keyboard_release_all(void) {
    g_kbd.modifiers = 0;
    memset(g_kbd.keycodes, 0, sizeof(g_kbd.keycodes));
    keyboard_mark_dirty();
}



static void radio_mark_dirty(void) {
    g_radio.dirty = true;
}

static void radio_reset(void) {
    memset(g_radio.axes, 0, sizeof(g_radio.axes));
    g_radio.axes[2] = cdc_axis_to_hid_i16(-1000);
    g_radio.buttons = 0;
    radio_mark_dirty();
}

static bool radio_button_valid(int n) {
    return n >= 1 && n <= 16;
}

static uint16_t radio_button_mask(int n) {
    return (uint16_t)(1u << (n - 1));
}

static void teleop_bump_sequence(void) {
    g_teleop.sequence++;
    if (g_teleop.sequence == 0) {
        g_teleop.sequence = 1;
    }
}

static void teleop_mark_bank_dirty(int bank) {
    if (bank < 0 || bank >= TELEOP_BANK_COUNT) return;
    g_teleop.bank_dirty[bank] = true;
    g_teleop.bank_sequence[bank] = g_teleop.sequence;
}

static void teleop_mark_all_dirty(void) {
    for (int bank = 0; bank < TELEOP_BANK_COUNT; ++bank) {
        teleop_mark_bank_dirty(bank);
    }
}

static void teleop_reset(void) {
    memset(g_teleop.axes, 0, sizeof(g_teleop.axes));
    teleop_bump_sequence();
    teleop_mark_all_dirty();
}

static bool keyboard_has_key(uint8_t keycode) {
    for (size_t idx = 0; idx < sizeof(g_kbd.keycodes); ++idx) {
        if (g_kbd.keycodes[idx] == keycode) return true;
    }
    return false;
}

static bool keyboard_add_key(uint8_t keycode) {
    if (keycode == 0) return true;
    if (keyboard_has_key(keycode)) return true;

    for (size_t idx = 0; idx < sizeof(g_kbd.keycodes); ++idx) {
        if (g_kbd.keycodes[idx] == 0) {
            g_kbd.keycodes[idx] = keycode;
            keyboard_mark_dirty();
            return true;
        }
    }

    return false;
}

static void keyboard_remove_key(uint8_t keycode) {
    for (size_t idx = 0; idx < sizeof(g_kbd.keycodes); ++idx) {
        if (g_kbd.keycodes[idx] == keycode) {
            g_kbd.keycodes[idx] = 0;
            keyboard_mark_dirty();
            return;
        }
    }
}

static void keyboard_set_modifiers(uint8_t modifiers) {
    if (g_kbd.modifiers != modifiers) {
        g_kbd.modifiers = modifiers;
        keyboard_mark_dirty();
    }
}

static void hid_release_all(void) {
    mouse_release_all();
    keyboard_release_all();
    radio_reset();
    teleop_reset();
}

static void send_status(void) {
    char msg[256];
    snprintf(
        msg, sizeof(msg),
        "STATUS mode=%s board=%s dp=%u dm=%u buttons=%u x=%d y=%d wheel=%d pan=%d kmod=0x%02X keys=%02X,%02X,%02X,%02X,%02X,%02X q=%u radio_btn=0x%04X teleop_seq=%u",
        hid_mode_name(g_hid_mode),
        current_board_profile()->display_name,
        current_board_profile()->pio_usb_dp_pin,
        current_board_profile()->pio_usb_dm_pin,
        g_mouse.buttons,
        g_mouse.x, g_mouse.y,
        g_mouse.wheel, g_mouse.pan,
        g_kbd.modifiers,
        g_kbd.keycodes[0], g_kbd.keycodes[1], g_kbd.keycodes[2],
        g_kbd.keycodes[3], g_kbd.keycodes[4], g_kbd.keycodes[5],
        (unsigned)((g_q_tail + ACTION_QUEUE_SIZE - g_q_head) % ACTION_QUEUE_SIZE),
        g_radio.buttons,
        g_teleop.sequence
    );
    cdc_write_line(msg);
}

static bool enqueue_delay(uint32_t delay_ms) {
    action_t a = {
        .type = ACT_DELAY_ONLY,
        .delay_ms = delay_ms
    };
    return queue_push(a);
}

static bool enqueue_set_buttons_after(uint32_t delay_ms, uint8_t buttons) {
    action_t a = {
        .type = ACT_SET_BUTTONS,
        .delay_ms = delay_ms,
        .buttons = (uint8_t)(buttons & 0x1F)
    };
    return queue_push(a);
}

static bool enqueue_report_after(uint32_t delay_ms, uint8_t buttons, int dx, int dy, int wheel, int pan) {
    action_t a = {
        .type = ACT_REPORT_REL,
        .delay_ms = delay_ms,
        .buttons = (uint8_t)(buttons & 0x1F),
        .dx = clamp_i8(dx),
        .dy = clamp_i8(dy),
        .wheel = clamp_i8(wheel),
        .pan = clamp_i8(pan)
    };
    return queue_push(a);
}

static bool button_valid(int n) {
    return n >= 1 && n <= 5;
}

static uint8_t button_mask(int n) {
    return (uint8_t)(1u << (n - 1));
}

static float smoothstepf_local(float t) {
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    return t * t * (3.0f - 2.0f * t);
}

static vec2f_t vec_add(vec2f_t a, vec2f_t b) {
    vec2f_t r = {a.x + b.x, a.y + b.y};
    return r;
}

static vec2f_t vec_sub(vec2f_t a, vec2f_t b) {
    vec2f_t r = {a.x - b.x, a.y - b.y};
    return r;
}

static vec2f_t vec_scale(vec2f_t a, float s) {
    vec2f_t r = {a.x * s, a.y * s};
    return r;
}

static float vec_len(vec2f_t a) {
    return sqrtf(a.x * a.x + a.y * a.y);
}

static vec2f_t vec_norm(vec2f_t a) {
    float l = vec_len(a);
    if (l < 1e-6f) {
        vec2f_t z = {0.0f, 0.0f};
        return z;
    }
    return vec_scale(a, 1.0f / l);
}

static vec2f_t vec_perp(vec2f_t a) {
    vec2f_t r = {-a.y, a.x};
    return r;
}

static vec2f_t bezier2(vec2f_t p0, vec2f_t p1, vec2f_t p2, float t) {
    float u = 1.0f - t;
    vec2f_t r = {
        u * u * p0.x + 2.0f * u * t * p1.x + t * t * p2.x,
        u * u * p0.y + 2.0f * u * t * p1.y + t * t * p2.y
    };
    return r;
}

static bool enqueue_smooth_path(uint8_t held_buttons,
                                int total_dx,
                                int total_dy,
                                int duration_ms,
                                int steps,
                                int curve_strength,
                                int overshoot_strength,
                                int jitter_strength,
                                int timing_jitter_ms,
                                bool final_correct) {
    if (steps < 2) steps = 2;
    if (duration_ms < 10) duration_ms = 10;
    if (timing_jitter_ms < 0) timing_jitter_ms = 0;
    if (curve_strength < 0) curve_strength = 0;
    if (overshoot_strength < 0) overshoot_strength = 0;
    if (jitter_strength < 0) jitter_strength = 0;

    vec2f_t start = {0.0f, 0.0f};
    vec2f_t end = {(float)total_dx, (float)total_dy};
    vec2f_t d = vec_sub(end, start);
    float dist = vec_len(d);

    if (dist < 0.5f) {
        return enqueue_report_after(0, held_buttons, total_dx, total_dy, 0, 0);
    }

    vec2f_t dir = vec_norm(d);
    vec2f_t normal = vec_perp(dir);

    int main_steps = steps;
    int corr_steps = 0;

    bool use_overshoot = (overshoot_strength > 0 && dist >= 60.0f);
    if (use_overshoot) {
        corr_steps = steps / 4;
        if (corr_steps < 4) corr_steps = 4;
        main_steps = steps - corr_steps;
        if (main_steps < 6) {
            main_steps = 6;
            corr_steps = 4;
        }
    }

    float base_curve = ((float)curve_strength) * 0.6f;
    float main_curve_mag = fminf(fmaxf(dist * 0.04f, 0.0f), 25.0f) * (base_curve / 10.0f);
    main_curve_mag *= rand_float(0.7f, 1.25f);
    if (rand_range(0, 1) == 0) main_curve_mag = -main_curve_mag;

    float low_freq_jitter_mag = 0.0f;
    if (jitter_strength > 0) {
        float local_scale = fminf(fmaxf(dist * 0.02f, 0.5f), 10.0f);
        low_freq_jitter_mag = local_scale * ((float)jitter_strength / 10.0f) * rand_float(0.6f, 1.2f);
        if (rand_range(0, 1) == 0) low_freq_jitter_mag = -low_freq_jitter_mag;
    }

    vec2f_t main_mid = vec_scale(vec_add(start, end), 0.5f);
    vec2f_t main_ctrl = vec_add(main_mid, vec_scale(normal, main_curve_mag));

    vec2f_t overshoot_target = end;
    vec2f_t corr_ctrl = end;

    float correction_jitter_mag = 0.0f;
    if (use_overshoot) {
        float overshoot_frac = 0.02f + 0.006f * (float)overshoot_strength;
        if (overshoot_frac > 0.10f) overshoot_frac = 0.10f;

        float over_forward = dist * overshoot_frac;
        float over_side = fminf(fmaxf(dist * 0.015f, 1.0f), 8.0f) *
                          (0.5f + 0.12f * (float)overshoot_strength);
        if (rand_range(0, 1) == 0) over_side = -over_side;

        overshoot_target = vec_add(end, vec_add(vec_scale(dir, over_forward),
                                                vec_scale(normal, over_side)));

        vec2f_t corr_mid = vec_scale(vec_add(overshoot_target, end), 0.5f);
        corr_ctrl = vec_add(corr_mid, vec_scale(normal, -over_side * 0.7f));

        if (jitter_strength > 0) {
            correction_jitter_mag = fminf(fmaxf(dist * 0.006f, 0.5f), 4.0f) *
                                    ((float)jitter_strength / 10.0f) *
                                    rand_float(0.8f, 1.4f);
            if (rand_range(0, 1) == 0) correction_jitter_mag = -correction_jitter_mag;
        }
    }

    vec2f_t prev = start;
    uint32_t base_dt = (uint32_t)(duration_ms / steps);
    if (base_dt < 1) base_dt = 1;
    bool ok = true;

    for (int i = 1; i <= main_steps; i++) {
        float t = (float)i / (float)main_steps;
        float u = smoothstepf_local(t);

        vec2f_t p_end = use_overshoot ? overshoot_target : end;
        vec2f_t p = bezier2(start, main_ctrl, p_end, u);

        if (jitter_strength > 0) {
            float envelope = sinf(3.1415926f * u);
            float local_j = low_freq_jitter_mag * envelope;
            p = vec_add(p, vec_scale(normal, local_j));
        }

        int step_dx = (int)lroundf(p.x - prev.x);
        int step_dy = (int)lroundf(p.y - prev.y);

        int dt = (int)base_dt + rand_range(-timing_jitter_ms, timing_jitter_ms);
        if (dt < 1) dt = 1;

        ok &= enqueue_report_after((uint32_t)dt, held_buttons, step_dx, step_dy, 0, 0);
        prev = vec_add(prev, (vec2f_t){(float)step_dx, (float)step_dy});
    }

    if (use_overshoot) {
        uint32_t corr_dt = (uint32_t)(duration_ms / steps);
        if (corr_dt < 1) corr_dt = 1;

        for (int i = 1; i <= corr_steps; i++) {
            float t = (float)i / (float)corr_steps;
            float u = smoothstepf_local(t);

            vec2f_t p = bezier2(overshoot_target, corr_ctrl, end, u);

            if (jitter_strength > 0) {
                float envelope = (1.0f - u) * sinf(2.0f * 3.1415926f * u);
                float local_j = correction_jitter_mag * envelope;
                p = vec_add(p, vec_scale(normal, local_j));
            }

            int step_dx = (int)lroundf(p.x - prev.x);
            int step_dy = (int)lroundf(p.y - prev.y);

            int dt = (int)corr_dt + rand_range(-timing_jitter_ms, timing_jitter_ms);
            if (dt < 1) dt = 1;

            ok &= enqueue_report_after((uint32_t)dt, held_buttons, step_dx, step_dy, 0, 0);
            prev = vec_add(prev, (vec2f_t){(float)step_dx, (float)step_dy});
        }
    }

    if (final_correct) {
        int final_dx = total_dx - (int)lroundf(prev.x);
        int final_dy = total_dy - (int)lroundf(prev.y);
        if (final_dx != 0 || final_dy != 0) {
            ok &= enqueue_report_after(1, held_buttons, final_dx, final_dy, 0, 0);
        }
    }

    return ok;
}



static int parse_int_tokens(char *line, int *values, int max_values) {
    int count = 0;
    char *tok = strtok(line, " \t");
    while (tok != NULL) {
        if (count >= max_values) {
            return -1;
        }

        char *end = NULL;
        long v = strtol(tok, &end, 0);
        if (end == tok || *end != '\0') {
            return -1;
        }
        values[count++] = (int)v;
        tok = strtok(NULL, " \t");
    }
    return count;
}

static bool parse_prefixed_ints(const char *line, const char *prefix, int *values, int max_values, int *count_out) {
    size_t n = strlen(prefix);
    if (strncmp(line, prefix, n) != 0) return false;
    if (line[n] != '\0' && line[n] != ' ' && line[n] != '\t') return false;

    char tmp[CDC_LINE_BUF_SIZE];
    strncpy(tmp, line + n, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';

    int count = parse_int_tokens(tmp, values, max_values);
    if (count < 0) return false;
    *count_out = count;
    return true;
}

static bool parse_mode_name(const char *s, hid_mode_t *mode_out) {
    if (strcmp(s, "BRIDGE") == 0 || strcmp(s, "MK") == 0 || strcmp(s, "MOUSE") == 0) {
        *mode_out = HID_MODE_BRIDGE;
        return true;
    }
    if (strcmp(s, "RADIO") == 0 || strcmp(s, "FPV") == 0 || strcmp(s, "JOYSTICK") == 0) {
        *mode_out = HID_MODE_RADIO;
        return true;
    }
    if (strcmp(s, "TELEOP") == 0) {
        *mode_out = HID_MODE_TELEOP;
        return true;
    }
    if (strcmp(s, "FULL") == 0 || strcmp(s, "EXPERIMENTAL") == 0) {
        *mode_out = HID_MODE_FULL;
        return true;
    }
    return false;
}

static void service_cdc_rx(void) {
    while (tud_cdc_available()) {
        uint8_t ch;
        if (tud_cdc_read(&ch, 1) != 1) break;

        if (ch == '\r') continue;

        if (ch == '\n') {
            g_line_buf[g_line_len] = '\0';
            g_line_len = 0;

            char *line = g_line_buf;
            while (*line == ' ' || *line == '\t') line++;
            if (*line == '\0') continue;

            if (strcmp(line, "PING") == 0) {
                cdc_write_line("PONG");
                continue;
            }

            if (strcmp(line, "STATUS") == 0) {
                send_status();
                continue;
            }

            if (strcmp(line, "MODE") == 0) {
                char msg[96];
                snprintf(msg, sizeof(msg), "MODE %s", hid_mode_name(g_hid_mode));
                cdc_write_line(msg);
                continue;
            }

            char mode_arg[24];
            if (sscanf(line, "MODE %23s", mode_arg) == 1) {
                hid_mode_t requested;
                if (!parse_mode_name(mode_arg, &requested)) {
                    cdc_write_line("ERR MODE");
                    continue;
                }
                if (requested == g_hid_mode) {
                    cdc_write_line("OK MODE UNCHANGED");
                    continue;
                }
                char msg[96];
                snprintf(msg, sizeof(msg), "OK MODE %s REBOOTING", hid_mode_name(requested));
                cdc_write_line(msg);
                mode_store_and_reboot(requested);
            }

            if (strcmp(line, "BOARD") == 0 || strcmp(line, "BOARD?") == 0) {
                send_board_status();
                continue;
            }

            char board_arg[32];
            if (sscanf(line, "BOARD %31s", board_arg) == 1) {
                if (parse_board_auto_name(board_arg)) {
                    board_type_t auto_board = g_auto_detected_board_type;
                    bool needs_reboot = auto_board != g_board_type;

                    if (g_board_select_source == BOARD_SELECT_SOURCE_AUTO && !needs_reboot) {
                        cdc_write_line("OK BOARD AUTO UNCHANGED");
                        continue;
                    }

                    if (!needs_reboot) {
                        board_clear_persistent_override();
                        g_board_select_source = BOARD_SELECT_SOURCE_AUTO;
                        cdc_write_line("OK BOARD AUTO SAVED");
                        continue;
                    }

                    const board_profile_t *profile = &BOARD_PROFILES[auto_board];
                    char msg[128];
                    snprintf(
                        msg,
                        sizeof(msg),
                        "OK BOARD AUTO %s REBOOTING",
                        profile->display_name
                    );
                    cdc_write_line(msg);
                    board_clear_and_reboot();
                }

                board_type_t requested;
                if (!parse_board_name(board_arg, &requested)) {
                    cdc_write_line("ERR BOARD");
                    continue;
                }

                const board_profile_t *profile = &BOARD_PROFILES[requested];
                if (requested == g_board_type) {
                    board_save_manual_override(requested);
                    g_board_select_source = BOARD_SELECT_SOURCE_MANUAL;

                    char msg[128];
                    snprintf(
                        msg,
                        sizeof(msg),
                        "OK BOARD %s SAVED",
                        profile->display_name
                    );
                    cdc_write_line(msg);
                    continue;
                }

                char msg[128];
                snprintf(
                    msg,
                    sizeof(msg),
                    "OK BOARD %s REBOOTING",
                    profile->display_name
                );
                cdc_write_line(msg);
                board_store_and_reboot(requested);
            }

            if (strcmp(line, "HEARTBEAT") == 0) {
                g_last_heartbeat = get_absolute_time();
                g_watchdog_tripped = false;
                cdc_write_line("OK HEARTBEAT");
                continue;
            }

            if (strcmp(line, "RESET") == 0) {
                hid_release_all();
                cdc_write_line("OK RESET");
                continue;
            }

            if (strcmp(line, "CANCEL_MOTION") == 0) {
                if (!mode_supports_bridge()) {
                    cdc_write_line("ERR MODE_UNSUPPORTED");
                    continue;
                }
                preempt_motion_plan(true);
                cdc_write_line("OK CANCEL_MOTION");
                continue;
            }

            if (strcmp(line, "RADIO_RESET") == 0) {
                if (!mode_supports_radio()) {
                    cdc_write_line("ERR MODE_UNSUPPORTED");
                    continue;
                }
                radio_reset();
                cdc_write_line("OK RADIO_RESET");
                continue;
            }

            if (strcmp(line, "TELEOP_RESET") == 0) {
                if (!mode_supports_teleop()) {
                    cdc_write_line("ERR MODE_UNSUPPORTED");
                    continue;
                }
                teleop_reset();
                cdc_write_line("OK TELEOP_RESET");
                continue;
            }

            int values[TELEOP_AXES_PER_BANK + 2];
            int value_count = 0;

            if (parse_prefixed_ints(line, "RADIO", values, RADIO_AXIS_COUNT, &value_count)) {
                if (!mode_supports_radio()) {
                    cdc_write_line("ERR MODE_UNSUPPORTED");
                    continue;
                }
                if (value_count < 4 || value_count > RADIO_AXIS_COUNT) {
                    cdc_write_line("ERR RADIO_ARGS");
                    continue;
                }
                for (int n = 0; n < RADIO_AXIS_COUNT; ++n) {
                    g_radio.axes[n] = (n < value_count) ? cdc_axis_to_hid_i16(values[n]) : 0;
                }
                radio_mark_dirty();
                cdc_write_line("OK RADIO");
                continue;
            }

            if (parse_prefixed_ints(line, "RADIO_BUTTONS", values, 1, &value_count)) {
                if (!mode_supports_radio()) {
                    cdc_write_line("ERR MODE_UNSUPPORTED");
                    continue;
                }
                if (value_count != 1 || values[0] < 0 || values[0] > 0xFFFF) {
                    cdc_write_line("ERR RADIO_BUTTONS");
                    continue;
                }
                g_radio.buttons = (uint16_t)values[0];
                radio_mark_dirty();
                cdc_write_line("OK RADIO_BUTTONS");
                continue;
            }

            if (parse_prefixed_ints(line, "RADIO_PRESS", values, 1, &value_count)) {
                if (!mode_supports_radio()) {
                    cdc_write_line("ERR MODE_UNSUPPORTED");
                    continue;
                }
                if (value_count != 1 || !radio_button_valid(values[0])) {
                    cdc_write_line("ERR RADIO_BUTTON");
                    continue;
                }
                g_radio.buttons |= radio_button_mask(values[0]);
                radio_mark_dirty();
                cdc_write_line("OK RADIO_PRESS");
                continue;
            }

            if (parse_prefixed_ints(line, "RADIO_RELEASE", values, 1, &value_count)) {
                if (!mode_supports_radio()) {
                    cdc_write_line("ERR MODE_UNSUPPORTED");
                    continue;
                }
                if (value_count != 1 || !radio_button_valid(values[0])) {
                    cdc_write_line("ERR RADIO_BUTTON");
                    continue;
                }
                g_radio.buttons &= (uint16_t)~radio_button_mask(values[0]);
                radio_mark_dirty();
                cdc_write_line("OK RADIO_RELEASE");
                continue;
            }

            if (parse_prefixed_ints(line, "TELEOP_AXIS", values, 2, &value_count)) {
                if (!mode_supports_teleop()) {
                    cdc_write_line("ERR MODE_UNSUPPORTED");
                    continue;
                }
                if (value_count != 2 || values[0] < 0 || values[0] >= TELEOP_AXIS_COUNT) {
                    cdc_write_line("ERR TELEOP_AXIS");
                    continue;
                }
                int axis = values[0];
                g_teleop.axes[axis] = cdc_axis_to_hid_i16(values[1]);
                teleop_bump_sequence();
                teleop_mark_bank_dirty(axis / TELEOP_AXES_PER_BANK);
                cdc_write_line("OK TELEOP_AXIS");
                continue;
            }

            if (parse_prefixed_ints(line, "TELEOP_BANK", values, TELEOP_AXES_PER_BANK + 1, &value_count)) {
                if (!mode_supports_teleop()) {
                    cdc_write_line("ERR MODE_UNSUPPORTED");
                    continue;
                }
                if (value_count != TELEOP_AXES_PER_BANK + 1 ||
                    values[0] < 0 || values[0] >= TELEOP_BANK_COUNT) {
                    cdc_write_line("ERR TELEOP_BANK");
                    continue;
                }
                int bank = values[0];
                int base = bank * TELEOP_AXES_PER_BANK;
                for (int n = 0; n < TELEOP_AXES_PER_BANK; ++n) {
                    g_teleop.axes[base + n] = cdc_axis_to_hid_i16(values[n + 1]);
                }
                teleop_bump_sequence();
                teleop_mark_bank_dirty(bank);
                cdc_write_line("OK TELEOP_BANK");
                continue;
            }

            int a = 0, b = 0, c = 0, d = 0, e = 0, f = 0, g = 0, h = 0, i = 0;

            if (sscanf(line, "MOVE %d %d", &a, &b) == 2) {
                if (!mode_supports_bridge()) { cdc_write_line("ERR MODE_UNSUPPORTED"); continue; }
                preempt_motion_plan(true);
                mouse_mark_report(g_mouse.buttons, a, b, 0, 0);
                cdc_write_line("OK MOVE");
                continue;
            }

            if (sscanf(line, "SCROLL %d %d", &a, &b) == 2) {
                if (!mode_supports_bridge()) { cdc_write_line("ERR MODE_UNSUPPORTED"); continue; }
                mouse_mark_report(g_mouse.buttons, 0, 0, a, b);
                cdc_write_line("OK SCROLL");
                continue;
            }

            if (sscanf(line, "BUTTONS %d", &a) == 1) {
                if (!mode_supports_bridge()) { cdc_write_line("ERR MODE_UNSUPPORTED"); continue; }
                g_motion_owns_button = false;
                g_motion_owned_mask = 0;
                mouse_mark_button_state((uint8_t)(a & 0x1F));
                cdc_write_line("OK BUTTONS");
                continue;
            }

            if (sscanf(line, "PRESS %d", &a) == 1) {
                if (!mode_supports_bridge()) { cdc_write_line("ERR MODE_UNSUPPORTED"); continue; }
                if (!button_valid(a)) {
                    cdc_write_line("ERR BUTTON");
                    continue;
                }
                g_motion_owns_button = false;
                g_motion_owned_mask = 0;
                mouse_mark_button_state((uint8_t)(g_mouse.buttons | button_mask(a)));
                cdc_write_line("OK PRESS");
                continue;
            }

            if (sscanf(line, "RELEASE %d", &a) == 1) {
                if (!mode_supports_bridge()) { cdc_write_line("ERR MODE_UNSUPPORTED"); continue; }
                if (!button_valid(a)) {
                    cdc_write_line("ERR BUTTON");
                    continue;
                }
                if (g_motion_owns_button && (g_motion_owned_mask & button_mask(a))) {
                    g_motion_owns_button = false;
                    g_motion_owned_mask = 0;
                }
                mouse_mark_button_state((uint8_t)(g_mouse.buttons & (uint8_t)~button_mask(a)));
                cdc_write_line("OK RELEASE");
                continue;
            }

            int parsed_click = sscanf(line, "CLICK %d %d %d %d %d %d", &a, &b, &c, &d, &e, &f);
            if (parsed_click >= 1) {
                if (!mode_supports_bridge()) { cdc_write_line("ERR MODE_UNSUPPORTED"); continue; }
                int button = a;
                int count = (parsed_click >= 2) ? b : 1;
                int interval_ms = (parsed_click >= 3) ? c : 120;
                int hold_ms = (parsed_click >= 4) ? d : 30;
                int interval_jitter_ms = (parsed_click >= 5) ? e : 0;
                int hold_jitter_ms = (parsed_click >= 6) ? f : 0;

                if (!button_valid(button)) {
                    cdc_write_line("ERR BUTTON");
                    continue;
                }
                if (count < 1) count = 1;
                if (interval_ms < 20) interval_ms = 20;
                if (hold_ms < 10) hold_ms = 10;
                if (interval_jitter_ms < 0) interval_jitter_ms = 0;
                if (hold_jitter_ms < 0) hold_jitter_ms = 0;

                uint8_t base_buttons = g_mouse.buttons;
                uint8_t click_mask = button_mask(button);

                bool ok = true;
                for (int n = 0; n < count; n++) {
                    int actual_hold = hold_ms + rand_range(-hold_jitter_ms, hold_jitter_ms);
                    if (actual_hold < 10) actual_hold = 10;

                    ok &= enqueue_set_buttons_after(0, (uint8_t)(base_buttons | click_mask));
                    ok &= enqueue_set_buttons_after((uint32_t)actual_hold, base_buttons);

                    if (n != count - 1) {
                        int actual_interval = interval_ms + rand_range(-interval_jitter_ms, interval_jitter_ms);
                        if (actual_interval < 20) actual_interval = 20;
                        ok &= enqueue_delay((uint32_t)actual_interval);
                    }
                }

                cdc_write_line(ok ? "OK CLICK" : "ERR QUEUE_FULL");
                continue;
            }

            int parsed_smooth = sscanf(line,
                                       "MOVE_SMOOTH %d %d %d %d %d %d %d %d %d",
                                       &a, &b, &c, &d, &e, &f, &g, &h, &i);
            if (parsed_smooth >= 4) {
                if (!mode_supports_bridge()) { cdc_write_line("ERR MODE_UNSUPPORTED"); continue; }
                int dx = a;
                int dy = b;
                int duration_ms = c;
                int steps = d;
                int curve = (parsed_smooth >= 5) ? e : 6;
                int overshoot = (parsed_smooth >= 6) ? f : 4;
                int jitter = (parsed_smooth >= 7) ? g : 2;
                int timing_jitter = (parsed_smooth >= 8) ? h : 1;
                bool final_correct = (parsed_smooth >= 9) ? (i != 0) : false;

                preempt_motion_plan(true);
                bool ok = enqueue_smooth_path(g_mouse.buttons, dx, dy, duration_ms, steps,
                                              curve, overshoot, jitter, timing_jitter, final_correct);
                cdc_write_line(ok ? "OK MOVE_SMOOTH" : "ERR QUEUE_FULL");
                continue;
            }

            int btn, dx, dy, duration_ms, steps, curve, overshoot, jitter, timing_jitter, final_correct_i;
            int parsed_drag = sscanf(line,
                                     "DRAG %d %d %d %d %d %d %d %d %d %d",
                                     &btn, &dx, &dy, &duration_ms, &steps,
                                     &curve, &overshoot, &jitter, &timing_jitter, &final_correct_i);
            if (parsed_drag >= 5) {
                if (!mode_supports_bridge()) { cdc_write_line("ERR MODE_UNSUPPORTED"); continue; }
                if (!button_valid(btn)) {
                    cdc_write_line("ERR BUTTON");
                    continue;
                }

                if (parsed_drag < 6) curve = 6;
                if (parsed_drag < 7) overshoot = 3;
                if (parsed_drag < 8) jitter = 2;
                if (parsed_drag < 9) timing_jitter = 1;
                bool final_correct = (parsed_drag >= 10) ? (final_correct_i != 0) : false;

                preempt_motion_plan(true);

                uint8_t base_buttons = g_mouse.buttons;
                uint8_t drag_buttons = (uint8_t)(base_buttons | button_mask(btn));

                g_motion_owns_button = true;
                g_motion_owned_mask = button_mask(btn);

                bool ok = true;
                ok &= enqueue_set_buttons_after(0, drag_buttons);
                ok &= enqueue_smooth_path(drag_buttons, dx, dy, duration_ms, steps,
                                          curve, overshoot, jitter, timing_jitter, final_correct);
                ok &= enqueue_set_buttons_after(1, base_buttons);

                if (!ok) {
                    g_motion_owns_button = false;
                    g_motion_owned_mask = 0;
                }

                cdc_write_line(ok ? "OK DRAG" : "ERR QUEUE_FULL");
                continue;
            }

            int k0, k1, k2, k3, k4, k5, k6;

            if (sscanf(line, "KEY_PRESS %d", &a) == 1) {
                if (!mode_supports_bridge()) { cdc_write_line("ERR MODE_UNSUPPORTED"); continue; }
                if (a < 0 || a > 0x65) {
                    cdc_write_line("ERR KEYCODE");
                    continue;
                }
                cdc_write_line(keyboard_add_key((uint8_t)a) ? "OK KEY_PRESS" : "ERR KEY_SLOTS_FULL");
                continue;
            }

            if (sscanf(line, "KEY_RELEASE %d", &a) == 1) {
                if (!mode_supports_bridge()) { cdc_write_line("ERR MODE_UNSUPPORTED"); continue; }
                if (a < 0 || a > 0x65) {
                    cdc_write_line("ERR KEYCODE");
                    continue;
                }
                keyboard_remove_key((uint8_t)a);
                cdc_write_line("OK KEY_RELEASE");
                continue;
            }

            if (sscanf(line, "MOD_PRESS %d", &a) == 1) {
                if (!mode_supports_bridge()) { cdc_write_line("ERR MODE_UNSUPPORTED"); continue; }
                if (a < 0 || a > 0xFF) {
                    cdc_write_line("ERR MODMASK");
                    continue;
                }
                keyboard_set_modifiers((uint8_t)(g_kbd.modifiers | (uint8_t)a));
                cdc_write_line("OK MOD_PRESS");
                continue;
            }

            if (sscanf(line, "MOD_RELEASE %d", &a) == 1) {
                if (!mode_supports_bridge()) { cdc_write_line("ERR MODE_UNSUPPORTED"); continue; }
                if (a < 0 || a > 0xFF) {
                    cdc_write_line("ERR MODMASK");
                    continue;
                }
                keyboard_set_modifiers((uint8_t)(g_kbd.modifiers & (uint8_t)(~(uint8_t)a)));
                cdc_write_line("OK MOD_RELEASE");
                continue;
            }

            if (strcmp(line, "KEY_RESET") == 0) {
                if (!mode_supports_bridge()) { cdc_write_line("ERR MODE_UNSUPPORTED"); continue; }
                keyboard_release_all();
                cdc_write_line("OK KEY_RESET");
                continue;
            }

            if (sscanf(line, "KEYBOARD %d %d %d %d %d %d %d", &k0, &k1, &k2, &k3, &k4, &k5, &k6) == 7) {
                if (!mode_supports_bridge()) { cdc_write_line("ERR MODE_UNSUPPORTED"); continue; }
                if (k0 < 0 || k0 > 0xFF ||
                    k1 < 0 || k1 > 0x65 ||
                    k2 < 0 || k2 > 0x65 ||
                    k3 < 0 || k3 > 0x65 ||
                    k4 < 0 || k4 > 0x65 ||
                    k5 < 0 || k5 > 0x65 ||
                    k6 < 0 || k6 > 0x65) {
                    cdc_write_line("ERR KEYBOARD_STATE");
                    continue;
                }

                g_kbd.modifiers = (uint8_t)k0;
                g_kbd.keycodes[0] = (uint8_t)k1;
                g_kbd.keycodes[1] = (uint8_t)k2;
                g_kbd.keycodes[2] = (uint8_t)k3;
                g_kbd.keycodes[3] = (uint8_t)k4;
                g_kbd.keycodes[4] = (uint8_t)k5;
                g_kbd.keycodes[5] = (uint8_t)k6;
                keyboard_mark_dirty();
                cdc_write_line("OK KEYBOARD");
                continue;
            }

            cdc_write_line("ERR UNKNOWN");
            continue;
        }

        if (g_line_len + 1 < CDC_LINE_BUF_SIZE) {
            g_line_buf[g_line_len++] = (char)ch;
        } else {
            g_line_len = 0;
            cdc_write_line("ERR LINE_TOO_LONG");
        }
    }
}

static void service_watchdog(void) {
    if (absolute_time_diff_us(g_last_heartbeat, get_absolute_time()) / 1000 > HEARTBEAT_TIMEOUT_MS) {
        if (!g_watchdog_tripped) {
            hid_release_all();
            g_watchdog_tripped = true;
            cdc_write_line("WATCHDOG RESET");
        }
    }
}

static void service_action_queue(void) {
    action_t a;

    if (!queue_peek(&a)) return;

    if (g_queue_deadline_ms == 0) {
        g_queue_deadline_ms = now_ms() + a.delay_ms;
    }

    if (now_ms() < g_queue_deadline_ms) {
        return;
    }

    if (g_mouse.dirty &&
        (a.type == ACT_SET_BUTTONS ||
         a.type == ACT_REPORT_REL ||
         a.type == ACT_RESET_ALL)) {
        return;
    }

    switch (a.type) {
        case ACT_DELAY_ONLY:
            break;

        case ACT_SET_BUTTONS:
            mouse_mark_report(a.buttons, 0, 0, 0, 0);
            if (g_motion_owns_button && !(a.buttons & g_motion_owned_mask)) {
                g_motion_owns_button = false;
                g_motion_owned_mask = 0;
            }
            break;

        case ACT_REPORT_REL:
            mouse_mark_report(a.buttons, a.dx, a.dy, a.wheel, a.pan);
            break;

        case ACT_RESET_ALL:
            mouse_release_all();
            break;

        default:
            break;
    }

    queue_pop();
    g_queue_deadline_ms = 0;
}

static void service_pio_mouse_tx(void) {
    if (!g_mouse.dirty) return;

    uint8_t report[6];
    report[0] = REPORT_ID_MOUSE;
    report[1] = g_mouse.buttons;
    report[2] = (uint8_t)g_mouse.x;
    report[3] = (uint8_t)g_mouse.y;
    report[4] = (uint8_t)g_mouse.wheel;
    report[5] = (uint8_t)g_mouse.pan;

    if (pio_usb_device_transfer(0x81, report, sizeof(report))) {
        g_mouse.x = 0;
        g_mouse.y = 0;
        g_mouse.wheel = 0;
        g_mouse.pan = 0;
        g_mouse.dirty = false;
    }
}

static void service_pio_keyboard_tx(void) {
    if (!g_kbd.dirty) return;
    if (g_mouse.dirty) return;

    uint8_t report[9];
    report[0] = REPORT_ID_KEYBOARD;
    report[1] = g_kbd.modifiers;
    report[2] = 0;
    memcpy(&report[3], g_kbd.keycodes, sizeof(g_kbd.keycodes));

    if (pio_usb_device_transfer(0x81, report, sizeof(report))) {
        g_kbd.dirty = false;
    }
}



static void service_pio_radio_tx(void) {
    if (!g_radio.dirty) return;

    if (g_hid_mode == HID_MODE_RADIO) {
        uint8_t report[RADIO_AXIS_COUNT * 2 + 2];
        for (int n = 0; n < RADIO_AXIS_COUNT; ++n) {
            uint16_t v = (uint16_t)g_radio.axes[n];
            report[n * 2] = (uint8_t)(v & 0xFF);
            report[n * 2 + 1] = (uint8_t)(v >> 8);
        }
        report[RADIO_AXIS_COUNT * 2] = (uint8_t)(g_radio.buttons & 0xFF);
        report[RADIO_AXIS_COUNT * 2 + 1] = (uint8_t)(g_radio.buttons >> 8);

        if (pio_usb_device_transfer(0x81, report, sizeof(report))) {
            g_radio.dirty = false;
        }
        return;
    }

    if (g_hid_mode == HID_MODE_FULL) {
        if (g_mouse.dirty || g_kbd.dirty) return;

        uint8_t report[1 + RADIO_AXIS_COUNT * 2 + 2];
        report[0] = REPORT_ID_RADIO;
        for (int n = 0; n < RADIO_AXIS_COUNT; ++n) {
            uint16_t v = (uint16_t)g_radio.axes[n];
            report[1 + n * 2] = (uint8_t)(v & 0xFF);
            report[2 + n * 2] = (uint8_t)(v >> 8);
        }
        report[1 + RADIO_AXIS_COUNT * 2] = (uint8_t)(g_radio.buttons & 0xFF);
        report[2 + RADIO_AXIS_COUNT * 2] = (uint8_t)(g_radio.buttons >> 8);

        if (pio_usb_device_transfer(0x81, report, sizeof(report))) {
            g_radio.dirty = false;
        }
    }
}

static void service_pio_teleop_tx(void) {
    if (!(g_hid_mode == HID_MODE_TELEOP || g_hid_mode == HID_MODE_FULL)) return;
    if (g_hid_mode == HID_MODE_FULL && (g_mouse.dirty || g_kbd.dirty || g_radio.dirty)) return;

    for (int attempt = 0; attempt < TELEOP_BANK_COUNT; ++attempt) {
        int bank = (g_teleop.next_bank_to_try + attempt) % TELEOP_BANK_COUNT;
        if (!g_teleop.bank_dirty[bank]) continue;

        uint8_t report_seq = g_teleop.bank_sequence[bank];
        int base = bank * TELEOP_AXES_PER_BANK;

        if (g_hid_mode == HID_MODE_TELEOP) {
            uint8_t report[1 + 1 + TELEOP_AXES_PER_BANK * 2];
            report[0] = (uint8_t)bank;
            report[1] = report_seq;
            for (int n = 0; n < TELEOP_AXES_PER_BANK; ++n) {
                uint16_t v = (uint16_t)g_teleop.axes[base + n];
                report[2 + n * 2] = (uint8_t)(v & 0xFF);
                report[3 + n * 2] = (uint8_t)(v >> 8);
            }

            if (pio_usb_device_transfer(0x81, report, sizeof(report))) {
                if (g_teleop.bank_sequence[bank] == report_seq) {
                    g_teleop.bank_dirty[bank] = false;
                }
                g_teleop.next_bank_to_try = (uint8_t)((bank + 1) % TELEOP_BANK_COUNT);
            }
            return;
        }

        if (g_hid_mode == HID_MODE_FULL) {
            uint8_t report[1 + 1 + 1 + TELEOP_AXES_PER_BANK * 2];
            report[0] = REPORT_ID_TELEOP_BANK;
            report[1] = (uint8_t)bank;
            report[2] = report_seq;
            for (int n = 0; n < TELEOP_AXES_PER_BANK; ++n) {
                uint16_t v = (uint16_t)g_teleop.axes[base + n];
                report[3 + n * 2] = (uint8_t)(v & 0xFF);
                report[4 + n * 2] = (uint8_t)(v >> 8);
            }

            if (pio_usb_device_transfer(0x81, report, sizeof(report))) {
                if (g_teleop.bank_sequence[bank] == report_seq) {
                    g_teleop.bank_dirty[bank] = false;
                }
                g_teleop.next_bank_to_try = (uint8_t)((bank + 1) % TELEOP_BANK_COUNT);
            }
            return;
        }
    }
}

static void service_pio_hid_tx(void) {
    if (mode_supports_bridge()) {
        service_pio_mouse_tx();
        service_pio_keyboard_tx();
    }
    if (mode_supports_radio()) {
        service_pio_radio_tx();
    }
    if (mode_supports_teleop()) {
        service_pio_teleop_tx();
    }
}

int main(void) {
    stdio_init_all();
    tusb_init();

    g_hid_mode = read_requested_mode();
    g_board_type = read_requested_board();

    pio_descs_set_mode(g_hid_mode);
    pio_descs_init();

    const board_profile_t *profile = current_board_profile();
    if (profile->pio_startup_delay_ms > 0) {
        sleep_ms(profile->pio_startup_delay_ms);
    }

    pio_usb_configuration_t pio_cfg = PIO_USB_DEFAULT_CONFIG;
    pio_cfg.pin_dp = profile->pio_usb_dp_pin;
    pio_cfg.pin_dm = profile->pio_usb_dm_pin;
    (void)pio_usb_device_init(&pio_cfg, &pio_descs);

    g_last_heartbeat = get_absolute_time();
    g_last_status = get_absolute_time();

    srand((unsigned)time_us_32());
    radio_reset();
    teleop_reset();

    while (true) {
        tud_task();
        pio_usb_device_task();

        if (tud_cdc_connected()) {
            service_cdc_rx();
        }

        service_watchdog();
        service_action_queue();
        service_pio_hid_tx();

        if (tud_cdc_connected() &&
            absolute_time_diff_us(g_last_status, get_absolute_time()) <= -1000000) {
            g_last_status = get_absolute_time();
            send_status();
        }
    }
}
