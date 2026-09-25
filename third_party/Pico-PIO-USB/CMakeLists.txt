# Vendored copy of Pico-PIO-USB (https://github.com/sekigon-gonnoc/Pico-PIO-USB).
# Licensed under the MIT License - see LICENSE in this directory.
#
# Only the library sources are kept here; the upstream examples and host tests
# are not needed by the VT100 terminal.

add_library(pico_pio_usb INTERFACE)

target_sources(pico_pio_usb INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/src/pio_usb.c
    ${CMAKE_CURRENT_LIST_DIR}/src/pio_usb.h
    ${CMAKE_CURRENT_LIST_DIR}/src/pio_usb_configuration.h
    ${CMAKE_CURRENT_LIST_DIR}/src/pio_usb_device.c
    ${CMAKE_CURRENT_LIST_DIR}/src/pio_usb_host.c
    ${CMAKE_CURRENT_LIST_DIR}/src/pio_usb_ll.h
    ${CMAKE_CURRENT_LIST_DIR}/src/usb_crc.c
    ${CMAKE_CURRENT_LIST_DIR}/src/usb_crc.h
    ${CMAKE_CURRENT_LIST_DIR}/src/usb_definitions.h
    )

target_include_directories(pico_pio_usb INTERFACE ${CMAKE_CURRENT_LIST_DIR}/src)

# The PIO programs are assembled into headers at configure time.
pico_generate_pio_header(pico_pio_usb ${CMAKE_CURRENT_LIST_DIR}/src/usb_rx.pio)
pico_generate_pio_header(pico_pio_usb ${CMAKE_CURRENT_LIST_DIR}/src/usb_tx.pio)
