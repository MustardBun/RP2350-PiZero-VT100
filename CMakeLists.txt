#
# RP2350 PiZero VT100 terminal
#
# Standalone build of the 04-VT100 application. The DVI and USB host libraries
# it needs are vendored under third_party/ so the repository is self-contained
# and needs no network access to build.
#
# Configure with PICO_SDK_PATH pointing at a Pico SDK 2.x checkout, e.g.
#   cmake -S . -B build -G Ninja -DPICO_SDK_PATH=/path/to/pico-sdk
#

cmake_minimum_required(VERSION 3.13)

# Board support. The Waveshare RP2350-PiZero header is shipped in boards/ and is
# added to the SDK's board search path, because the SDK release in use may not
# carry it yet.
set(PICO_BOARD_HEADER_DIRS ${CMAKE_CURRENT_LIST_DIR}/boards)
set(PICO_BOARD waveshare_rp2350_pizero)

include(${CMAKE_CURRENT_LIST_DIR}/pico_sdk_import.cmake)

project(vt100_terminal C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# The whole image runs from RAM, so core 1's DVI scanout never fetches from
# flash. See the PICO_FLASH_ASSUME_CORE1_SAFE note below.
set(PICO_COPY_TO_RAM 1)

pico_sdk_init()

# pico_sock_cfg is the DVI pin mapping used by the Waveshare RP2350-PiZero HDMI
# socket. Override with -DDVI_DEFAULT_SERIAL_CONFIG=<name> for another board.
set(DVI_DEFAULT_SERIAL_CONFIG "pico_sock_cfg" CACHE STRING "DVI pin configuration")

# PIO pin numbers are addressed as absolute GPIO numbers rather than
# state-machine-relative offsets.
add_compile_definitions(PICO_PIO_USE_GPIO_BASE=1)

add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/third_party/PicoDVI/libdvi libdvi)
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/third_party/PicoDVI/libgui libgui)
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/third_party/Pico-PIO-USB pico_pio_usb)

add_executable(vt100_terminal
    src/main.c
    src/fonts_vt100.c
    ${PICO_TINYUSB_PATH}/src/portable/raspberrypi/pio_usb/hcd_pio_usb.c
)

# The strict warning set applies to this project's own sources. Vendored
# third-party code is compiled as-is, so local warning flags never mask the
# upstream sources or complicate diffing against them.
set_source_files_properties(
    src/main.c
    src/fonts_vt100.c
    PROPERTIES COMPILE_OPTIONS "-Wall;-Wextra"
)

# PICO_COPY_TO_RAM puts the whole image in RAM, so core 1's DVI scanout never
# fetches from flash. Declaring that lets flash_safe_execute() skip the
# multicore lockout: parking core 1 stalls scanout for the length of the sector
# erase, which most monitors see as a lost signal. Without the lockout only the
# calling core's interrupts are suspended and the picture is undisturbed.
# Keep this in step with PICO_COPY_TO_RAM - if code runs from flash again,
# removing it would corrupt scanout.
target_compile_definitions(vt100_terminal PRIVATE
    DVI_DEFAULT_SERIAL_CONFIG=${DVI_DEFAULT_SERIAL_CONFIG}
    DVI_VERTICAL_REPEAT=1
    DVI_N_TMDS_BUFFERS=3
    PIO_USB_USE_TINYUSB
    PICO_FLASH_ASSUME_CORE1_SAFE=1
)

target_include_directories(vt100_terminal PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/src
    ${CMAKE_CURRENT_LIST_DIR}/third_party/PicoDVI/include
)

# TinyUSB looks for tusb_config.h by bare name, so the directory holding it has
# to be on the include path rather than only next to the sources.
target_include_directories(vt100_terminal PRIVATE ${CMAKE_CURRENT_LIST_DIR}/src)

target_link_libraries(vt100_terminal PRIVATE
    pico_stdlib
    pico_multicore
    pico_flash
    hardware_flash
    libdvi
    libgui
    pico_pio_usb
    tinyusb_host
)

# The terminal uses UART0 and the PIO-USB port directly, so the SDK's own stdio
# drivers must stay out of the way.
pico_enable_stdio_uart(vt100_terminal 0)
pico_enable_stdio_usb(vt100_terminal 0)

pico_add_extra_outputs(vt100_terminal)
