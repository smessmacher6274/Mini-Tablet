cmake_minimum_required(VERSION 3.13)

set(PICO_BOARD pico_w CACHE STRING "Target board")
if(NOT PICO_SDK_PATH AND DEFINED ENV{PICO_SDK_PATH})
    set(PICO_SDK_PATH "$ENV{PICO_SDK_PATH}")
endif()
if(NOT EXISTS "${PICO_SDK_PATH}/external/pico_sdk_import.cmake")
    message(FATAL_ERROR "Set PICO_SDK_PATH to a recursive checkout of raspberrypi/pico-sdk (see README.md).")
endif()
include("${PICO_SDK_PATH}/external/pico_sdk_import.cmake")

project(picow_bluetooth C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
pico_sdk_init()

if(NOT TARGET pico_btstack_ble OR NOT TARGET pico_btstack_cyw43)
    message(FATAL_ERROR "Bluetooth support missing. Use PICO_BOARD=pico_w and initialize SDK submodules.")
endif()

add_executable(picow_bluetooth main.c display.c)
target_include_directories(picow_bluetooth PRIVATE ${CMAKE_CURRENT_LIST_DIR})
target_link_libraries(picow_bluetooth PRIVATE
    pico_stdlib pico_btstack_ble pico_btstack_cyw43 pico_cyw43_arch_none hardware_spi)
pico_btstack_make_gatt_header(picow_bluetooth PRIVATE "${CMAKE_CURRENT_LIST_DIR}/service.gatt")
pico_enable_stdio_usb(picow_bluetooth 1)
pico_enable_stdio_uart(picow_bluetooth 0)
pico_add_extra_outputs(picow_bluetooth)
