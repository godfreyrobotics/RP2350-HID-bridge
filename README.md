# RP2350 HID Bridge

Firmware for an RP2350-based computer-to-computer HID bridge that receives CDC commands on the native USB port and emulates USB HID output on a second USB interface implemented using PIO USB.

## Overview

This project supports the **Waveshare RP2350-PiZero** and **Waveshare RP2350-USB-A** boards.

The same firmware image can run on both boards using automatic board detection:

- **Native USB interface:** CDC command/control input
- **PIO USB interface:** HID output to the target computer

A controller computer sends commands to the RP2350 over CDC, and the RP2350 appears to the target computer as a real USB input device.

The firmware supports multiple HID output modes, including a normal mouse/keyboard bridge mode, an FPV radio/joystick mode, a teleoperation mode, and an experimental all-in-one mode.

## Hardware setup

Typical connection model:

### Waveshare RP2350-PiZero

- **Controller computer** -> connected to the **native USB** port
- **Target computer** -> connected to the **PIO USB** port

### Waveshare RP2350-USB-A

- **Controller computer** -> connected to the **native USB** port
- **Target computer** -> connected to the **USB-A output connector**

The controller computer sends text-based commands over CDC, and the target computer sees the RP2350 as a USB HID device.

For development and testing, the HID output can also be plugged back into the **same laptop** that is sending the CDC commands.

## Features

### HID output modes

- RAM-selected HID output modes
- Automatic board detection for supported Waveshare RP2350 boards
- Optional persistent manual board profile override
- Mode switching over CDC
- Mode switching reboots the device so the PIO USB side can re-enumerate with a new HID descriptor
- No flash writes are used for normal mode switching

### Mouse

- Relative mouse movement
- Preemptive motion commands: new motion commands interrupt older active motion plans
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

### FPV radio / joystick

- Simple radio/joystick HID mode
- 8 analog channels
- 16 buttons
- Useful for FPV simulators and games that expect a standard controller

### Teleoperation

- Vendor-defined teleop HID mode
- 60 analog axes
- 2 banks
- 30 axes per bank
- Sequence byte per bank update
- Intended for custom robotics/teleoperation software

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

After flashing, the firmware should automatically detect the correct board profile. You can verify the active board profile with:

```bash
printf 'BOARD?\n' | sudo tee /dev/ttyACM0 > /dev/null
```

## USB architecture

This project uses the board's two USB paths for separate roles:

- **Native USB**
  - Enumerates as a CDC serial/control interface
  - Receives text commands from the controller computer

- **PIO USB**
  - Enumerates as the selected HID output mode
  - Sends mouse, keyboard, joystick/radio, or teleop HID reports to the target computer depending on the active mode

This keeps the control channel separate from the HID output side.

## Board selection

The firmware supports automatic board detection and optional persistent manual board overrides.

By default, the firmware attempts to select the correct board profile during boot. Current auto-detection uses detected flash size:

- large flash, such as 16 MB -> Waveshare RP2350-PiZero
- small flash, such as 2 MB -> Waveshare RP2350-USB-A

Manual board commands are optional. They are useful if you want to force a profile or return to automatic detection after setting an override.

### Board commands

#### `BOARD?`

Prints the current board profile, pin configuration, selection source, auto-detected board, and detected flash size.

```text
BOARD?
```

Example responses:

```text
BOARD waveshare_rp2350_usb_a dp=12 dm=13 source=auto auto=waveshare_rp2350_usb_a flash=2097152
```

```text
BOARD waveshare_rp2350_pizero dp=28 dm=29 source=auto auto=waveshare_rp2350_pizero flash=16777216
```

#### `BOARD AUTO`

Clears any persistent manual override and returns to automatic board detection.

```text
BOARD AUTO
```

#### `BOARD PIZERO`

Persistently forces the Waveshare RP2350-PiZero board profile.

```text
BOARD PIZERO
```

#### `BOARD USBA`

Persistently forces the Waveshare RP2350-USB-A board profile.

```text
BOARD USBA
```

Changing board profiles causes the RP2350 to reboot so that the PIO USB side can reinitialize using the correct USB pin configuration.

## HID output modes

The firmware supports multiple PIO USB HID output modes. The selected mode is stored in RAM/watchdog scratch state, not flash, so switching modes does not consume flash write cycles.

Changing modes causes the RP2350 to reboot so that the PIO USB side can re-enumerate with a different USB HID descriptor. This is necessary because USB descriptors are read by the target computer when the device connects.

### Available modes

#### `BRIDGE`

Default mouse/keyboard bridge mode.

PIO USB exposes:

- USB mouse
- USB keyboard

Use this mode for normal computer-to-computer HID automation.

#### `RADIO`

FPV simulator / joystick mode.

PIO USB exposes:

- USB joystick / FPV radio controller
- 8 analog channels
- 16 buttons

Use this mode for FPV simulators or games that expect a standard controller.

#### `TELEOP`

Teleoperation mode.

PIO USB exposes:

- Vendor-defined teleoperation HID device
- 60 analog axes
- 2 banks
- 30 axes per bank
- sequence byte per bank update

This mode is intended for custom robotics/teleoperation software that reads HID reports directly.

#### `FULL`

Experimental all-in-one mode.

PIO USB exposes:

- mouse
- keyboard
- radio controller
- teleop report

This mode is useful for Linux/custom testing, but may not be compatible with Windows games or simulators.

### Mode commands

#### `MODE`

Prints the current HID output mode.

```text
MODE
```

#### `MODE BRIDGE`

Switches to normal mouse/keyboard bridge mode and reboots.

```text
MODE BRIDGE
```

#### `MODE RADIO`

Switches to FPV radio/joystick mode and reboots.

```text
MODE RADIO
```

#### `MODE TELEOP`

Switches to teleoperation HID mode and reboots.

```text
MODE TELEOP
```

#### `MODE FULL`

Switches to the experimental combined HID mode and reboots.

```text
MODE FULL
```

Example:

```bash
printf 'MODE RADIO\n' | sudo tee /dev/ttyACM0 > /dev/null
```

After switching modes, the CDC serial port may disconnect and reconnect.

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
MODE RADIO
RADIO 0 0 -1000 0 0 0 0 0
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

Releases all input state and cancels active scheduled input activity.

```text
RESET
```

## Mouse commands

Mouse commands are available in `BRIDGE` mode and `FULL` mode.

### `MOVE dx dy`

Performs immediate relative mouse movement. This command is **preemptive**: it cancels any active smooth motion or drag plan before applying the new movement.

Parameters:

- `dx`: relative X movement
- `dy`: relative Y movement

Example:

```text
MOVE 100 0
```

### `MOVE_SMOOTH dx dy duration_ms steps [curve] [overshoot] [jitter] [timing_jitter] [final_correct]`

Performs smooth relative movement over time. This command is **preemptive**: it cancels any active smooth motion or drag plan before applying the new movement.

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

Holds a mouse button and performs smooth relative movement. This command is **preemptive**: it cancels any active smooth motion or drag plan before applying the new movement.

Parameters:

- `button`: mouse button to hold during drag
- remaining parameters follow the same meaning as `MOVE_SMOOTH`

Example:

```text
DRAG 1 250 0 1000 40
```

### `CANCEL_MOTION`

Cancels the active motion plan immediately.

```text
CANCEL_MOTION
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

Keyboard commands are available in `BRIDGE` mode and `FULL` mode.

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

## Radio commands

Radio commands are available in `RADIO` mode and `FULL` mode.

Channel order:

```text
RADIO roll pitch throttle yaw aux1 aux2 aux3 aux4
```

Values are normalized from `-1000` to `1000`.

### `RADIO ch1 ch2 ch3 ch4 [ch5 ch6 ch7 ch8]`

Sets the current radio channel state.

Example neutral sticks with low throttle:

```text
RADIO 0 0 -1000 0 0 0 0 0
```

Example roll right:

```text
RADIO 1000 0 -1000 0 0 0 0 0
```

### `RADIO_RESET`

Resets radio channels to a safe state.

```text
RADIO_RESET
```

Safe reset state:

- roll = `0`
- pitch = `0`
- throttle = `-1000`
- yaw = `0`
- aux channels = `0`
- buttons off

### `RADIO_BUTTONS mask`

Sets the full 16-bit radio button mask.

```text
RADIO_BUTTONS 0x0001
```

### `RADIO_PRESS button`

Presses a radio button.

```text
RADIO_PRESS 1
```

### `RADIO_RELEASE button`

Releases a radio button.

```text
RADIO_RELEASE 1
```

## Teleop commands

Teleop commands are available in `TELEOP` mode and `FULL` mode.

The teleop HID report exposes 60 signed analog axes:

- bank 0: axes 0-29
- bank 1: axes 30-59

Values are normalized from `-1000` to `1000`.

### `TELEOP_AXIS index value`

Sets one teleop axis.

Example:

```text
TELEOP_AXIS 0 1000
```

Example for bank 1:

```text
TELEOP_AXIS 30 -500
```

### `TELEOP_BANK bank v0 v1 ... v29`

Sets all 30 axes in a bank.

Example:

```text
TELEOP_BANK 0 1000 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 -1000
```

### `TELEOP_RESET`

Resets all teleop axes to zero.

```text
TELEOP_RESET
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

### Check current board profile

```bash
printf 'BOARD?\n' | sudo tee /dev/ttyACM0 > /dev/null
```

### Return to automatic board detection

```bash
printf 'BOARD AUTO\n' | sudo tee /dev/ttyACM0 > /dev/null
```

### Force the RP2350-USB-A board profile

```bash
printf 'BOARD USBA\n' | sudo tee /dev/ttyACM0 > /dev/null
```

### Force the RP2350-PiZero board profile

```bash
printf 'BOARD PIZERO\n' | sudo tee /dev/ttyACM0 > /dev/null
```

### Check current HID mode

```bash
printf 'MODE\n' | sudo tee /dev/ttyACM0 > /dev/null
```

### Switch to radio mode

```bash
printf 'MODE RADIO\n' | sudo tee /dev/ttyACM0 > /dev/null
```

### Switch back to bridge mode

```bash
printf 'MODE BRIDGE\n' | sudo tee /dev/ttyACM0 > /dev/null
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

### FPV radio neutral state

```bash
printf 'RADIO 0 0 -1000 0 0 0 0 0\n' | sudo tee /dev/ttyACM0 > /dev/null
```

### FPV radio stick circle

This moves the roll/pitch stick in a circle while keeping throttle low and yaw centered.

```bash
while true; do
  printf 'RADIO 1000 0 -1000 0 0 0 0 0\n' | sudo tee /dev/ttyACM0 > /dev/null
  sleep 0.15
  printf 'RADIO 707 707 -1000 0 0 0 0 0\n' | sudo tee /dev/ttyACM0 > /dev/null
  sleep 0.15
  printf 'RADIO 0 1000 -1000 0 0 0 0 0\n' | sudo tee /dev/ttyACM0 > /dev/null
  sleep 0.15
  printf 'RADIO -707 707 -1000 0 0 0 0 0\n' | sudo tee /dev/ttyACM0 > /dev/null
  sleep 0.15
  printf 'RADIO -1000 0 -1000 0 0 0 0 0\n' | sudo tee /dev/ttyACM0 > /dev/null
  sleep 0.15
  printf 'RADIO -707 -707 -1000 0 0 0 0 0\n' | sudo tee /dev/ttyACM0 > /dev/null
  sleep 0.15
  printf 'RADIO 0 -1000 -1000 0 0 0 0 0\n' | sudo tee /dev/ttyACM0 > /dev/null
  sleep 0.15
  printf 'RADIO 707 -707 -1000 0 0 0 0 0\n' | sudo tee /dev/ttyACM0 > /dev/null
  sleep 0.15
done
```

### Teleop axis test

```bash
printf 'TELEOP_AXIS 0 1000\n' | sudo tee /dev/ttyACM0 > /dev/null
printf 'TELEOP_AXIS 30 -1000\n' | sudo tee /dev/ttyACM0 > /dev/null
```

## Safety behavior

This firmware is designed to fail in a safer way than blindly holding input state forever.

- A controller can periodically send `HEARTBEAT`
- If heartbeats stop, the watchdog can release active input state
- `RESET` releases all input state
- `KEY_RESET` releases all keyboard state
- `RADIO_RESET` resets radio channels to a safe state
- `TELEOP_RESET` resets teleop axes to zero
- `CANCEL_MOTION` stops active motion immediately
- New motion commands are preemptive and replace older active motion plans
- Mouse button state can also be cleared explicitly with `BUTTONS 0`

This helps prevent stuck keys, stuck modifiers, stuck mouse buttons, or stale controller state if the controller software crashes or disconnects.

## Current limitations

- Pointer movement is **relative-only** in firmware
- Standard 6-key boot keyboard behavior only
- Mode switching requires reboot/re-enumeration
- `FULL` mode is experimental and may not be compatible with Windows games/simulators
- Board auto-detection currently relies on flash size heuristics
- Higher-level macros, motion planning, CV, and autonomy are intended to live on the controller computer, not in firmware

## Future work

- Controller-side macro layer
- Integration with computer vision / autonomous control loops
- Improved FPV simulator host tooling
- Host-side teleop configuration/mapping layer
- Separate documentation for HID descriptors and report formats
- Improved hardware-level board identification
- Additional RP2350 board support

## Hardware

- Waveshare RP2350-PiZero
- Waveshare RP2350-USB-A

## Notes

- Placeholder VID/PIDs are currently used for development
- HID mode switching is RAM-based and does not write to flash
- Board overrides are persistent and stored separately from HID mode selection
