# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

# Console (printf/logs) on the XIAO is already over USB CDC ACM: the xiao_ble board
# enables CONFIG_BOARD_SERIAL_BACKEND_CDC_ACM by default. Don't add the
# cdc-acm-console snippet on top: it creates a second, unexposed CDC ACM port and
# steals zephyr,console from the one the host actually sees.
# On the nRF5340 DK the console is the J-Link virtual COM port (UART0).

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(master_node)

target_sources(app PRIVATE
    src/main.c
    src/camera_node.c
    src/camera_link.c
    src/image_store.c
    src/cam_wake.c
    src/noon_clock.c
)
