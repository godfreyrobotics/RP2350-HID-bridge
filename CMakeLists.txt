cmake_minimum_required(VERSION 3.13)

set(PICO_BOARD pico2)

include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(rp2350_hid_bridge C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

if(NOT DEFINED USB_MANUFACTURER_STRING)
    set(USB_MANUFACTURER_STRING "Godfrey")
endif()

if(NOT DEFINED USB_CONTROL_PRODUCT_STRING)
    set(USB_CONTROL_PRODUCT_STRING "USB Bridge Control")
endif()

if(NOT DEFINED USB_POINTER_PRODUCT_STRING)
    set(USB_POINTER_PRODUCT_STRING "USB Bridge Pointer")
endif()

if(NOT DEFINED USB_INTERFACE_STRING)
    set(USB_INTERFACE_STRING "Command Port")
endif()

if(NOT DEFINED USB_RADIO_PRODUCT_STRING)
    set(USB_RADIO_PRODUCT_STRING "Virtual FPV Radio")
endif()

if(NOT DEFINED USB_TELEOP_PRODUCT_STRING)
    set(USB_TELEOP_PRODUCT_STRING "Teleop HID")
endif()

if(NOT DEFINED USB_FULL_PRODUCT_STRING)
    set(USB_FULL_PRODUCT_STRING "USB Bridge Full Experimental")
endif()

if(NOT DEFINED USB_SHARED_SERIAL_STRING)
    set(USB_SHARED_SERIAL_STRING "BRIDGE0001")
endif()

pico_sdk_init()

add_subdirectory(src/pio_usb_lib)

add_executable(rp2350_hid_bridge
    src/main.c
    src/native_usb_descriptors.c
    src/pio_mouse_descriptors.c
)

target_include_directories(rp2350_hid_bridge PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/src
    ${PICO_SDK_PATH}/lib/tinyusb/src
    ${PICO_SDK_PATH}/lib/tinyusb/hw
)

target_compile_definitions(rp2350_hid_bridge PRIVATE
    USB_MANUFACTURER_STRING="${USB_MANUFACTURER_STRING}"
    USB_CONTROL_PRODUCT_STRING="${USB_CONTROL_PRODUCT_STRING}"
    USB_POINTER_PRODUCT_STRING="${USB_POINTER_PRODUCT_STRING}"
    USB_INTERFACE_STRING="${USB_INTERFACE_STRING}"
    USB_RADIO_PRODUCT_STRING="${USB_RADIO_PRODUCT_STRING}"
    USB_TELEOP_PRODUCT_STRING="${USB_TELEOP_PRODUCT_STRING}"
    USB_FULL_PRODUCT_STRING="${USB_FULL_PRODUCT_STRING}"
    USB_SHARED_SERIAL_STRING="${USB_SHARED_SERIAL_STRING}"
)

target_link_libraries(rp2350_hid_bridge
    pico_stdlib
    pico_multicore
    hardware_pio
    hardware_dma
    hardware_flash
    tinyusb_device
    tinyusb_board
    pico_pio_usb
)

pico_enable_stdio_uart(rp2350_hid_bridge 1)
pico_enable_stdio_usb(rp2350_hid_bridge 0)

pico_add_extra_outputs(rp2350_hid_bridge)
