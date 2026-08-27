#define HID_BRIDGE_HOST_TEST
#include "../src/main.c"

#include <stdio.h>

#define HOST_BUFFER_SIZE 32768u

static absolute_time_t host_now_us = 0;
static bool host_cdc_is_connected = true;
static uint8_t host_rx[HOST_BUFFER_SIZE];
static size_t host_rx_length = 0;
static size_t host_rx_offset = 0;
static char host_tx[HOST_BUFFER_SIZE];
static size_t host_tx_length = 0;
static uint32_t host_tx_window = 64;
static uint32_t host_tx_window_available = 64;
static int64_t host_mouse_dx = 0;
static int64_t host_mouse_dy = 0;
static uint8_t host_mouse_buttons = 0;
static unsigned host_mouse_reports = 0;

uint8_t host_xip_flash[2u * 1024u * 1024u];
static host_watchdog_hw_t host_watchdog_hw = {0};
host_watchdog_hw_t *watchdog_hw = &host_watchdog_hw;
usb_descriptor_buffers_t pio_descs = {0};

static int failures = 0;

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        failures++; \
    } \
} while (0)

absolute_time_t get_absolute_time(void) { return host_now_us; }
int64_t absolute_time_diff_us(absolute_time_t from, absolute_time_t to) { return to - from; }
absolute_time_t make_timeout_time_ms(uint32_t delay_ms) {
    return host_now_us + (absolute_time_t)delay_ms * 1000;
}
bool time_reached(absolute_time_t target) { return host_now_us >= target; }
uint32_t time_us_32(void) { return (uint32_t)host_now_us; }
void sleep_ms(uint32_t delay_ms) { host_now_us += (absolute_time_t)delay_ms * 1000; }
void tight_loop_contents(void) {}
void stdio_init_all(void) {}
void watchdog_reboot(uint32_t pc, uint32_t sp, uint32_t delay_ms) {
    (void)pc; (void)sp; (void)delay_ms;
}

void flash_do_cmd(const uint8_t *txbuf, uint8_t *rxbuf, size_t count) {
    (void)txbuf;
    memset(rxbuf, 0, count);
    if (count >= 4) rxbuf[3] = 21;
}
void flash_range_erase(uint32_t offset, size_t count) {
    memset(&host_xip_flash[offset], 0xFF, count);
}
void flash_range_program(uint32_t offset, const uint8_t *data, size_t count) {
    memcpy(&host_xip_flash[offset], data, count);
}
uint32_t save_and_disable_interrupts(void) { return 0; }
void restore_interrupts(uint32_t status) { (void)status; }

const char *hid_mode_name(hid_mode_t mode) {
    static const char *names[] = {"BRIDGE", "RADIO", "TELEOP", "FULL"};
    return mode < HID_MODE_COUNT ? names[mode] : "UNKNOWN";
}
hid_mode_t hid_mode_from_u32(uint32_t value) {
    return value < HID_MODE_COUNT ? (hid_mode_t)value : HID_MODE_BRIDGE;
}
void pio_descs_init(void) {}
void pio_descs_set_mode(hid_mode_t mode) { (void)mode; }
usb_device_t *pio_usb_device_init(const pio_usb_configuration_t *config,
                                  const usb_descriptor_buffers_t *buffers) {
    (void)config; (void)buffers;
    return NULL;
}
void pio_usb_device_task(void) {}

bool pio_usb_device_transfer(uint8_t endpoint, uint8_t *buffer, uint16_t length) {
    (void)endpoint;
    if (length == 6 && buffer[0] == REPORT_ID_MOUSE) {
        host_mouse_buttons = buffer[1];
        host_mouse_dx += (int8_t)buffer[2];
        host_mouse_dy += (int8_t)buffer[3];
        host_mouse_reports++;
    }
    return true;
}

void tusb_init(void) {}
void tud_task(void) {}
bool tud_cdc_connected(void) { return host_cdc_is_connected; }
uint32_t tud_cdc_available(void) { return (uint32_t)(host_rx_length - host_rx_offset); }
uint32_t tud_cdc_read(void *buffer, uint32_t size) {
    size_t remaining = host_rx_length - host_rx_offset;
    size_t count = size < remaining ? size : remaining;
    memcpy(buffer, &host_rx[host_rx_offset], count);
    host_rx_offset += count;
    return (uint32_t)count;
}
uint32_t tud_cdc_write_available(void) { return host_tx_window_available; }
uint32_t tud_cdc_write(const void *buffer, uint32_t size) {
    uint32_t count = size < host_tx_window_available ? size : host_tx_window_available;
    CHECK(host_tx_length + count < HOST_BUFFER_SIZE);
    memcpy(&host_tx[host_tx_length], buffer, count);
    host_tx_length += count;
    host_tx_window_available -= count;
    return count;
}
uint32_t tud_cdc_write_flush(void) {
    host_tx_window_available = host_tx_window;
    return 0;
}

static void reset_fixture(void) {
    memset(&g_mouse, 0, sizeof(g_mouse));
    memset(&g_kbd, 0, sizeof(g_kbd));
    memset(&g_radio, 0, sizeof(g_radio));
    memset(&g_teleop, 0, sizeof(g_teleop));
    memset(g_queue, 0, sizeof(g_queue));
    memset(g_line_buf, 0, sizeof(g_line_buf));
    memset(g_cdc_tx_queue, 0, sizeof(g_cdc_tx_queue));
    g_hid_mode = HID_MODE_BRIDGE;
    g_board_type = BOARD_WAVESHARE_RP2350_USB_A;
    g_auto_detected_board_type = BOARD_WAVESHARE_RP2350_USB_A;
    g_board_select_source = BOARD_SELECT_SOURCE_AUTO;
    g_detected_flash_size_bytes = 2u * 1024u * 1024u;
    g_line_len = 0;
    g_discard_line_until_newline = false;
    g_cdc_tx_head = 0;
    g_cdc_tx_tail = 0;
    g_q_head = 0;
    g_q_tail = 0;
    g_queue_deadline_set = false;
    g_motion_owns_button = false;
    g_motion_owned_mask = 0;
    g_watchdog_tripped = false;
    host_now_us = 0;
    g_last_heartbeat = host_now_us;
    host_cdc_is_connected = true;
    host_rx_length = 0;
    host_rx_offset = 0;
    host_tx_length = 0;
    host_tx_window = 64;
    host_tx_window_available = host_tx_window;
    host_mouse_dx = 0;
    host_mouse_dy = 0;
    host_mouse_buttons = 0;
    host_mouse_reports = 0;
    srand(1);
}

static void drain_tx(void) {
    unsigned guard = 0;
    while (cdc_tx_depth() > 0 && guard++ < 10000) {
        service_cdc_tx();
    }
    CHECK(cdc_tx_depth() == 0);
    host_tx[host_tx_length] = '\0';
}

static void clear_tx(void) {
    drain_tx();
    host_tx_length = 0;
    host_tx[0] = '\0';
}

static void feed_bytes(const void *data, size_t length) {
    CHECK(length < HOST_BUFFER_SIZE);
    memcpy(host_rx, data, length);
    host_rx_length = length;
    host_rx_offset = 0;

    unsigned guard = 0;
    while (host_rx_offset < host_rx_length && guard++ < 10000) {
        service_cdc_rx();
        service_cdc_tx();
    }
    CHECK(host_rx_offset == host_rx_length);
    drain_tx();
}

static void feed_line(const char *line) {
    feed_bytes(line, strlen(line));
}

static void test_complete_cdc_lines(void) {
    reset_fixture();
    host_tx_window = 7;
    host_tx_window_available = host_tx_window;

    feed_line("STATUS\n");
    CHECK(strncmp(host_tx, "STATUS mode=BRIDGE", 18) == 0);
    CHECK(strstr(host_tx, "teleop_seq=0\r\n") != NULL);

    clear_tx();
    feed_line("BOARD?\n");
    CHECK(strstr(host_tx, "source=auto") != NULL);
    CHECK(strstr(host_tx, "flash=2097152\r\n") != NULL);
}

static void test_watchdog_rearms_after_every_control_command(void) {
    reset_fixture();
    host_now_us = 2001000;
    service_watchdog();
    CHECK(g_watchdog_tripped);

    feed_line("KEY_PRESS 4\n");
    CHECK(!g_watchdog_tripped);
    CHECK(keyboard_has_key(4));

    host_now_us += 2001001;
    service_watchdog();
    CHECK(g_watchdog_tripped);
    CHECK(!keyboard_has_key(4));

    feed_line("MOD_PRESS 1\n");
    CHECK(!g_watchdog_tripped);
    CHECK(g_kbd.modifiers == 1);

    host_now_us += 2001001;
    service_watchdog();
    CHECK(g_watchdog_tripped);
    CHECK(g_kbd.modifiers == 0);
}

static void test_composite_queue_failures_are_atomic(void) {
    reset_fixture();
    for (int index = 0; index < 510; ++index) {
        CHECK(enqueue_delay(1));
    }
    CHECK(queue_depth() == 510);
    feed_line("CLICK 1\n");
    CHECK(strstr(host_tx, "ERR QUEUE_FULL\r\n") != NULL);
    CHECK(queue_depth() == 510);

    reset_fixture();
    feed_line("MOVE_SMOOTH 65000 0 1000 2 0 0 0 0 1\n");
    CHECK(strstr(host_tx, "ERR QUEUE_FULL\r\n") != NULL);
    CHECK(queue_empty());
    CHECK(!g_motion_owns_button);

    reset_fixture();
    g_mouse.buttons = 1;
    feed_line("DRAG 1 65000 0 1000 2 0 0 0 0 1\n");
    CHECK(strstr(host_tx, "ERR QUEUE_FULL\r\n") != NULL);
    CHECK(queue_empty());
    CHECK(!g_motion_owns_button);
    CHECK(g_mouse.buttons == 0);
}

static void test_smooth_motion_preserves_large_displacement(void) {
    reset_fixture();
    feed_line("MOVE_SMOOTH 1000 0 1000 2 0 0 0 0 1\n");
    CHECK(strstr(host_tx, "OK MOVE_SMOOTH\r\n") != NULL);

    unsigned guard = 0;
    while ((!queue_empty() || g_mouse.dirty) && guard++ < 5000) {
        host_now_us += 1000000;
        service_action_queue();
        service_pio_mouse_tx();
    }
    CHECK(queue_empty());
    CHECK(!g_mouse.dirty);
    CHECK(host_mouse_dx == 1000);
    CHECK(host_mouse_dy == 0);
    CHECK(host_mouse_reports > 2);
}

static void test_overlong_input_discards_the_entire_line(void) {
    reset_fixture();
    char line[CDC_LINE_BUF_SIZE + 64];
    memset(line, 'X', sizeof(line));
    const char suffix[] = "KEY_PRESS 4\n";
    memcpy(&line[sizeof(line) - sizeof(suffix)], suffix, sizeof(suffix) - 1);
    line[sizeof(line) - 1] = '\n';

    feed_bytes(line, sizeof(line));
    CHECK(strstr(host_tx, "ERR LINE_TOO_LONG\r\n") != NULL);
    CHECK(!keyboard_has_key(4));

    clear_tx();
    feed_line("KEY_PRESS 4\n");
    CHECK(strstr(host_tx, "OK KEY_PRESS\r\n") != NULL);
    CHECK(keyboard_has_key(4));
}

static void test_numeric_commands_require_exact_safe_arguments(void) {
    reset_fixture();
    feed_line("MOVE 1 2 trailing\n");
    CHECK(strstr(host_tx, "ERR MOVE_ARGS\r\n") != NULL);
    CHECK(!g_mouse.dirty);

    clear_tx();
    feed_line("CLICK 1 999999999999999999999\n");
    CHECK(strstr(host_tx, "ERR CLICK_ARGS\r\n") != NULL);
    CHECK(queue_empty());

    clear_tx();
    feed_line("BUTTONS -1\n");
    CHECK(strstr(host_tx, "ERR BUTTONS_MASK\r\n") != NULL);
    CHECK(g_mouse.buttons == 0);
}

static void test_explicit_button_state_preempts_drag(void) {
    reset_fixture();
    feed_line("DRAG 1 100 0 1000 10 0 0 0 0 1\n");
    CHECK(!queue_empty());
    CHECK(g_motion_owns_button);

    clear_tx();
    feed_line("RELEASE 1\n");
    CHECK(strstr(host_tx, "OK RELEASE\r\n") != NULL);
    CHECK(queue_empty());
    CHECK(!g_motion_owns_button);
    CHECK(g_mouse.buttons == 0);
}

static void test_queue_status_and_long_uptime_deadline(void) {
    reset_fixture();
    feed_line("QUEUE?\n");
    CHECK(strcmp(host_tx, "QUEUE depth=0 active=0 idle=1\r\n") == 0);

    clear_tx();
    CHECK(enqueue_delay(10));
    feed_line("QUEUE?\n");
    CHECK(strcmp(host_tx, "QUEUE depth=1 active=1 idle=0\r\n") == 0);

    host_now_us = ((absolute_time_t)UINT32_MAX + 1000) * 1000;
    service_action_queue();
    CHECK(!queue_empty());
    host_now_us += 9000;
    service_action_queue();
    CHECK(!queue_empty());
    host_now_us += 1000;
    service_action_queue();
    CHECK(queue_empty());
}

int main(void) {
    test_complete_cdc_lines();
    test_watchdog_rearms_after_every_control_command();
    test_composite_queue_failures_are_atomic();
    test_smooth_motion_preserves_large_displacement();
    test_overlong_input_discards_the_entire_line();
    test_numeric_commands_require_exact_safe_arguments();
    test_explicit_button_state_preempts_drag();
    test_queue_status_and_long_uptime_deadline();

    if (failures != 0) {
        fprintf(stderr, "%d test assertion(s) failed\n", failures);
        return 1;
    }

    puts("All firmware host tests passed.");
    return 0;
}
