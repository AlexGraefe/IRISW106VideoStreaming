cmake_minimum_required(VERSION 3.20.0)

set(ZEPHYR_EXTRA_MODULES 
    "${CMAKE_SOURCE_DIR}/modules/wifi_utilities" 
    "${CMAKE_SOURCE_DIR}/modules/udp_socket"
    "${CMAKE_SOURCE_DIR}/modules/n6_communication"
)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(demo_wifi)

target_sources(app PRIVATE 
    src/main.c
)