#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

#include "pico/stdlib.h"
#include "tusb.h"

#include "pio_usb.h"
#include "pio_usb_configuration.h"
#include "usb_definitions.h"

extern usb_descriptor_buffers_t pio_descs;
void pio_descs_init(void);
bool pio_usb_device_transfer(uint8_t ep_address, uint8_t *buffer, uint16_t buflen);

#define CDC_LINE_BUF_SIZE 192
#define HEARTBEAT_TIMEOUT_MS 2000
#define ACTION_QUEUE_SIZE 512

enum {
    REPORT_ID_MOUSE = 0x01,
    REPORT_ID_KEYBOARD = 0x02,
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
static char g_line_buf[CDC_LINE_BUF_SIZE];
static size_t g_line_len = 0;
static absolute_time_t g_last_heartbeat;
static absolute_time_t g_last_status;
static bool g_watchdog_tripped = false;

static action_t g_queue[ACTION_QUEUE_SIZE];
static uint16_t g_q_head = 0;
static uint16_t g_q_tail = 0;
static uint32_t g_queue_deadline_ms = 0;

static inline int8_t clamp_i8(int v) {
    if (v < -127) return -127;
    if (v > 127) return 127;
    return (int8_t)v;
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
}

static void keyboard_mark_dirty(void) {
    g_kbd.dirty = true;
}

static void keyboard_release_all(void) {
    g_kbd.modifiers = 0;
    memset(g_kbd.keycodes, 0, sizeof(g_kbd.keycodes));
    keyboard_mark_dirty();
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
}

static void send_status(void) {
    char msg[224];
    snprintf(
        msg, sizeof(msg),
        "STATUS buttons=%u x=%d y=%d wheel=%d pan=%d kmod=0x%02X keys=%02X,%02X,%02X,%02X,%02X,%02X q=%u",
        g_mouse.buttons,
        g_mouse.x, g_mouse.y,
        g_mouse.wheel, g_mouse.pan,
        g_kbd.modifiers,
        g_kbd.keycodes[0], g_kbd.keycodes[1], g_kbd.keycodes[2],
        g_kbd.keycodes[3], g_kbd.keycodes[4], g_kbd.keycodes[5],
        (unsigned)((g_q_tail + ACTION_QUEUE_SIZE - g_q_head) % ACTION_QUEUE_SIZE)
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

            int a = 0, b = 0, c = 0, d = 0, e = 0, f = 0, g = 0, h = 0, i = 0;

            if (sscanf(line, "MOVE %d %d", &a, &b) == 2) {
                mouse_mark_report(g_mouse.buttons, a, b, 0, 0);
                cdc_write_line("OK MOVE");
                continue;
            }

            if (sscanf(line, "SCROLL %d %d", &a, &b) == 2) {
                mouse_mark_report(g_mouse.buttons, 0, 0, a, b);
                cdc_write_line("OK SCROLL");
                continue;
            }

            if (sscanf(line, "BUTTONS %d", &a) == 1) {
                mouse_mark_button_state((uint8_t)(a & 0x1F));
                cdc_write_line("OK BUTTONS");
                continue;
            }

            if (sscanf(line, "PRESS %d", &a) == 1) {
                if (!button_valid(a)) {
                    cdc_write_line("ERR BUTTON");
                    continue;
                }
                mouse_mark_button_state((uint8_t)(g_mouse.buttons | button_mask(a)));
                cdc_write_line("OK PRESS");
                continue;
            }

            if (sscanf(line, "RELEASE %d", &a) == 1) {
                if (!button_valid(a)) {
                    cdc_write_line("ERR BUTTON");
                    continue;
                }
                mouse_mark_button_state((uint8_t)(g_mouse.buttons & (uint8_t)~button_mask(a)));
                cdc_write_line("OK RELEASE");
                continue;
            }

            int parsed_click = sscanf(line, "CLICK %d %d %d %d %d %d", &a, &b, &c, &d, &e, &f);
            if (parsed_click >= 1) {
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
                int dx = a;
                int dy = b;
                int duration_ms = c;
                int steps = d;
                int curve = (parsed_smooth >= 5) ? e : 6;
                int overshoot = (parsed_smooth >= 6) ? f : 4;
                int jitter = (parsed_smooth >= 7) ? g : 2;
                int timing_jitter = (parsed_smooth >= 8) ? h : 1;
                bool final_correct = (parsed_smooth >= 9) ? (i != 0) : false;

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
                if (!button_valid(btn)) {
                    cdc_write_line("ERR BUTTON");
                    continue;
                }

                if (parsed_drag < 6) curve = 6;
                if (parsed_drag < 7) overshoot = 3;
                if (parsed_drag < 8) jitter = 2;
                if (parsed_drag < 9) timing_jitter = 1;
                bool final_correct = (parsed_drag >= 10) ? (final_correct_i != 0) : false;

                uint8_t base_buttons = g_mouse.buttons;
                uint8_t drag_buttons = (uint8_t)(base_buttons | button_mask(btn));

                bool ok = true;
                ok &= enqueue_set_buttons_after(0, drag_buttons);
                ok &= enqueue_smooth_path(drag_buttons, dx, dy, duration_ms, steps,
                                          curve, overshoot, jitter, timing_jitter, final_correct);
                ok &= enqueue_set_buttons_after(1, base_buttons);

                cdc_write_line(ok ? "OK DRAG" : "ERR QUEUE_FULL");
                continue;
            }

            int k0, k1, k2, k3, k4, k5, k6;

            if (sscanf(line, "KEY_PRESS %d", &a) == 1) {
                if (a < 0 || a > 0x65) {
                    cdc_write_line("ERR KEYCODE");
                    continue;
                }
                cdc_write_line(keyboard_add_key((uint8_t)a) ? "OK KEY_PRESS" : "ERR KEY_SLOTS_FULL");
                continue;
            }

            if (sscanf(line, "KEY_RELEASE %d", &a) == 1) {
                if (a < 0 || a > 0x65) {
                    cdc_write_line("ERR KEYCODE");
                    continue;
                }
                keyboard_remove_key((uint8_t)a);
                cdc_write_line("OK KEY_RELEASE");
                continue;
            }

            if (sscanf(line, "MOD_PRESS %d", &a) == 1) {
                if (a < 0 || a > 0xFF) {
                    cdc_write_line("ERR MODMASK");
                    continue;
                }
                keyboard_set_modifiers((uint8_t)(g_kbd.modifiers | (uint8_t)a));
                cdc_write_line("OK MOD_PRESS");
                continue;
            }

            if (sscanf(line, "MOD_RELEASE %d", &a) == 1) {
                if (a < 0 || a > 0xFF) {
                    cdc_write_line("ERR MODMASK");
                    continue;
                }
                keyboard_set_modifiers((uint8_t)(g_kbd.modifiers & (uint8_t)(~(uint8_t)a)));
                cdc_write_line("OK MOD_RELEASE");
                continue;
            }

            if (strcmp(line, "KEY_RESET") == 0) {
                keyboard_release_all();
                cdc_write_line("OK KEY_RESET");
                continue;
            }

            if (sscanf(line, "KEYBOARD %d %d %d %d %d %d %d", &k0, &k1, &k2, &k3, &k4, &k5, &k6) == 7) {
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

int main(void) {
    stdio_init_all();
    tusb_init();

    pio_descs_init();

    const pio_usb_configuration_t pio_cfg = PIO_USB_DEFAULT_CONFIG;
    (void)pio_usb_device_init(&pio_cfg, &pio_descs);

    g_last_heartbeat = get_absolute_time();
    g_last_status = get_absolute_time();

    srand((unsigned)time_us_32());

    while (true) {
        tud_task();
        pio_usb_device_task();

        if (tud_cdc_connected()) {
            service_cdc_rx();
        }

        service_watchdog();
        service_action_queue();
        service_pio_mouse_tx();
        service_pio_keyboard_tx();

        if (tud_cdc_connected() &&
            absolute_time_diff_us(g_last_status, get_absolute_time()) <= -1000000) {
            g_last_status = get_absolute_time();
            send_status();
        }
    }
}
