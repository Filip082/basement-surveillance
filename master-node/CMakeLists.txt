# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

# Console (printf/logs) over USB instead of the D6/D7 UART pins
set(SNIPPET cdc-acm-console)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(master_node)

target_sources(app PRIVATE src/main.c)
