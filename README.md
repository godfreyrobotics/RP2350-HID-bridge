# RP2350 HID Bridge

Firmware for an RP2350-based computer-to-computer HID bridge that receives CDC commands on the native USB port and emulates USB mouse/keyboard input on the PIO USB port.

## Overview

This project targets the **Waveshare RP2350-PiZero** and uses its dual-USB setup to bridge two computers:

- **Native USB port:** CDC command/control input
- **PIO USB port:** HID mouse/keyboard output

A controller computer sends commands to the RP2350 over CDC, and the RP2350 appears to the target computer as a real USB input device.

## Hardware setup

Typical connection model:

- **Controller computer** -> connected to the **native USB** port
- **Target computer** -> connected to the **PIO USB** port

The controller computer sends text-based commands over CDC, and the target computer sees the RP2350 as a USB mouse/keyboard.

For development and testing, the HID output can also be plugged back into the **same laptop** that is sending the CDC commands.

## Features

### Mouse

- Relative mouse movement
- Smooth relative mouse movement
- Button press / release
- Click and double click
- Drag operations
- Vertical scroll wheel
- Horizontal pan / scroll
- Full mouse button mask control

### Keyboard

- Raw key press / release
- Modifier press / release
- Full keyboard state set
- Keyboard reset
- Standard 6-key boot keyboard behavior

### Reliability / control

- CDC command protocol
- Heartbeat support
- Watchdog timeout
- Reset-all-inputs behavior
- Status reporting

## Build instructions

### Requirements

- Raspberry Pi Pico SDK installed locally
- `PICO_SDK_PATH` set correctly
- CMake and a working ARM toolchain

Example Pico SDK setup:

```bash
cd ~
git clone https://github.com/raspberrypi/pico-sdk.git
cd pico-sdk
git submodule update --init --recursive
echo 'export PICO_SDK_PATH=$HOME/pico-sdk' >> ~/.bashrc
source ~/.bashrc
```

### Build

From the project directory:

```bash
rm -rf build
mkdir build
cd build
cmake ..
make -j$(nproc)
```

This should produce firmware outputs including a `.uf2` file in the build directory.

## Flashing the firmware

### Enter BOOTSEL mode

To flash new firmware onto the RP2350 board:

1. Disconnect the board from USB if it is already plugged in
2. Hold the **BOOTSEL** button on the board
3. While holding it, plug in the USB cable used for flashing
4. Release the button once a mass-storage device appears on your computer

The board should appear as a removable USB drive.

### Flash using UF2 copy

After building, copy the generated `.uf2` file to the mounted BOOTSEL drive.

Example:

```bash
cp build/rp2350_hid_bridge.uf2 /media/$USER/RPI-RP2/
```

The exact mount path may vary depending on your Linux distribution and desktop environment.

Once the copy finishes, the board should automatically reboot into the new firmware.

## USB architecture

This project uses the board’s two USB paths for separate roles:

- **Native USB**
  - Enumerates as a CDC serial/control interface
  - Receives text commands from the controller computer

- **PIO USB**
  - Enumerates as a HID device
  - Currently exposes mouse and keyboard functionality to the target computer

This keeps the control channel separate from the HID output side.

## CDC command protocol

Commands are sent as newline-terminated text lines over the CDC port.

General form:

```text
COMMAND arg1 arg2 arg3 ...
```

Examples:

```text
MOVE 100 0
CLICK 1
KEY_PRESS 4
MOD_PRESS 1
```

### General commands

#### `PING`

Checks whether the device is responsive.

```text
PING
```

#### `STATUS`

Returns current state/debug information.

```text
STATUS
```

#### `HEARTBEAT`

Refreshes the watchdog timeout.

```text
HEARTBEAT
```

#### `RESET`

Releases all mouse buttons, clears keyboard state, and cancels active scheduled input activity.

```text
RESET
```

## Mouse commands

### `MOVE dx dy`

Performs immediate relative mouse movement.

Parameters:

- `dx`: relative X movement
- `dy`: relative Y movement

Example:

```text
MOVE 100 0
```

### `MOVE_SMOOTH dx dy duration_ms steps [curve] [overshoot] [jitter] [timing_jitter] [final_correct]`

Performs smooth relative movement over time.

Parameters:

- `dx`: total relative X movement
- `dy`: total relative Y movement
- `duration_ms`: total motion duration in milliseconds
- `steps`: number of movement steps
- `curve`: optional motion curve parameter
- `overshoot`: optional overshoot amount
- `jitter`: optional spatial jitter
- `timing_jitter`: optional timing jitter
- `final_correct`: optional final correction enable/flag

Example:

```text
MOVE_SMOOTH 300 0 1000 40
```

### `DRAG button dx dy duration_ms steps [curve] [overshoot] [jitter] [timing_jitter] [final_correct]`

Holds a mouse button and performs smooth relative movement.

Parameters:

- `button`: mouse button to hold during drag
- remaining parameters follow the same meaning as `MOVE_SMOOTH`

Example:

```text
DRAG 1 250 0 1000 40
```

### `CLICK button [count] [interval_ms] [hold_ms] [interval_jitter_ms] [hold_jitter_ms]`

Performs one or more clicks.

Parameters:

- `button`: button to click
- `count`: optional number of clicks
- `interval_ms`: optional delay between clicks
- `hold_ms`: optional press duration
- `interval_jitter_ms`: optional interval jitter
- `hold_jitter_ms`: optional hold jitter

Examples:

```text
CLICK 1
CLICK 1 2 120 40 0 0
```

### `PRESS button`

Presses and holds a mouse button.

Example:

```text
PRESS 1
```

### `RELEASE button`

Releases a mouse button.

Example:

```text
RELEASE 1
```

### `BUTTONS mask`

Sets the full mouse button state directly.

Example:

```text
BUTTONS 0
BUTTONS 1
```

### `SCROLL wheel pan`

Sends vertical and horizontal scroll input.

Parameters:

- `wheel`: vertical scroll
- `pan`: horizontal scroll

Examples:

```text
SCROLL 5 0
SCROLL 0 -3
```

## Keyboard commands

### `KEY_PRESS keycode`

Presses a non-modifier key.

Example:

```text
KEY_PRESS 4
```

### `KEY_RELEASE keycode`

Releases a non-modifier key.

Example:

```text
KEY_RELEASE 4
```

### `MOD_PRESS mask`

Presses one or more modifier bits.

Example:

```text
MOD_PRESS 1
```

### `MOD_RELEASE mask`

Releases one or more modifier bits.

Example:

```text
MOD_RELEASE 1
```

### `KEYBOARD mods k1 k2 k3 k4 k5 k6`

Sets the full boot-keyboard state directly.

Parameters:

- `mods`: modifier bitmask
- `k1` through `k6`: up to 6 simultaneous non-modifier HID keycodes

Example:

```text
KEYBOARD 3 4 5 0 0 0 0
```

### `KEY_RESET`

Releases all keyboard keys and modifiers.

Example:

```text
KEY_RESET
```

## Modifier bitmask reference

Standard HID boot keyboard modifier bits:

- `1` = Left Ctrl
- `2` = Left Shift
- `4` = Left Alt
- `8` = Left GUI
- `16` = Right Ctrl
- `32` = Right Shift
- `64` = Right Alt
- `128` = Right GUI

Examples:

- `MOD_PRESS 1` = hold Left Ctrl
- `MOD_PRESS 3` = hold Left Ctrl + Left Shift
- `MOD_PRESS 8` = hold Left GUI

## Example usage

These examples assume the CDC port is available as `/dev/ttyACM0`.

### Basic status

```bash
printf 'STATUS\n' | sudo tee /dev/ttyACM0 > /dev/null
```

### Move the mouse right

```bash
printf 'MOVE 100 0\n' | sudo tee /dev/ttyACM0 > /dev/null
```

### Smooth movement

```bash
printf 'MOVE_SMOOTH 300 0 1000 40\n' | sudo tee /dev/ttyACM0 > /dev/null
```

### Left click

```bash
printf 'CLICK 1\n' | sudo tee /dev/ttyACM0 > /dev/null
```

### Drag with left mouse button

```bash
printf 'DRAG 1 250 0 1000 40\n' | sudo tee /dev/ttyACM0 > /dev/null
```

### Vertical scroll

```bash
printf 'SCROLL 5 0\n' | sudo tee /dev/ttyACM0 > /dev/null
```

### Press and release `A`

```bash
printf 'KEY_PRESS 4\n' | sudo tee /dev/ttyACM0 > /dev/null
printf 'KEY_RELEASE 4\n' | sudo tee /dev/ttyACM0 > /dev/null
```

### Send Ctrl+C

```bash
printf 'MOD_PRESS 1\n' | sudo tee /dev/ttyACM0 > /dev/null
printf 'KEY_PRESS 6\n' | sudo tee /dev/ttyACM0 > /dev/null
printf 'KEY_RELEASE 6\n' | sudo tee /dev/ttyACM0 > /dev/null
printf 'MOD_RELEASE 1\n' | sudo tee /dev/ttyACM0 > /dev/null
```

### Send Win+R

```bash
printf 'MOD_PRESS 8\n' | sudo tee /dev/ttyACM0 > /dev/null
printf 'KEY_PRESS 21\n' | sudo tee /dev/ttyACM0 > /dev/null
printf 'KEY_RELEASE 21\n' | sudo tee /dev/ttyACM0 > /dev/null
printf 'MOD_RELEASE 8\n' | sudo tee /dev/ttyACM0 > /dev/null
```

## Safety behavior

This firmware is designed to fail in a safer way than blindly holding input state forever.

- A controller can periodically send `HEARTBEAT`
- If heartbeats stop, the watchdog can release active input state
- `RESET` releases all mouse and keyboard state
- `KEY_RESET` releases all keyboard state
- Mouse button state can also be cleared explicitly with `BUTTONS 0`

This helps prevent stuck keys, stuck modifiers, or stuck mouse buttons if the controller software crashes or disconnects.

## Current limitations

- Pointer movement is **relative-only** in firmware
- Standard 6-key boot keyboard behavior only
- Higher-level macros and motion planning are intended to live on the controller computer, not in firmware

## Future work

- Controller-side macro layer
- Integration with computer vision / autonomous control loops

## Hardware

- Waveshare RP2350-PiZero

## Notes

- Placeholder VID/PIDs are currently used for development
