# RP2350 HID Bridge

Firmware for an RP2350-based computer-to-computer HID bridge that receives CDC commands on the native USB port and emulates USB mouse/keyboard input on the PIO USB port.

## Overview

This project targets the Waveshare RP2350-PiZero and uses its dual-USB setup to bridge two computers:

- **Native USB port:** CDC command/control input
- **PIO USB port:** HID mouse/keyboard output

A controller computer sends commands to the RP2350 over CDC, and the RP2350 appears to the target computer as a real USB mouse/keyboard device.

## Current features

### Mouse
- Relative mouse movement
- Smooth relative movement
- Click / press / release
- Drag
- Vertical scroll
- Horizontal pan
- Button mask control

### Keyboard
- Raw key press/release
- Modifier press/release
- Full keyboard state set
- Keyboard reset
- Standard 6-key boot keyboard behavior

### Reliability / control
- CDC command protocol
- Heartbeat support
- Watchdog timeout
- Reset-all-inputs behavior
- Status reporting

## Design direction

- Keep firmware simple and robust
- Keep pointer movement relative-only in firmware
- Put higher-level motion planning, macros, and computer vision logic on the controller computer

## Build

Requires the Pico SDK:

```bash
export PICO_SDK_PATH=$HOME/pico-sdk
rm -rf build
mkdir build
cd build
cmake ..
make -j$(nproc)

##Hardware
- Waveshare RP2350-PiZero

##Notes
- Placeholder VID/PIDs are currently used for development.
