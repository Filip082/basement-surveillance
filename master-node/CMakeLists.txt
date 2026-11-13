# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)

# Preserve USB console rather than using the D6/D7 UART pins.
set(SNIPPET cdc-acm-console)
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
