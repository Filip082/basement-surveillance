# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)

# xiao_ble enables CONFIG_BOARD_SERIAL_BACKEND_CDC_ACM by default, so logs
# already use USB. Adding cdc-acm-console creates a second CDC ACM device and
# redirects zephyr,console away from the port registered by the board backend.
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(master_node)

target_include_directories(app PRIVATE include)
target_sources(app PRIVATE
  src/main.c
  src/protocol.c
  src/tx_session.c
  src/lora_transport.c
)
target_sources_ifdef(CONFIG_APP_PERSISTENT_IDS app PRIVATE src/message_ids.c)
