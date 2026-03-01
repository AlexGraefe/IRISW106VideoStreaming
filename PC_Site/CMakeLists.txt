cmake_minimum_required(VERSION 3.10)
project(SocketDemo C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)



# Compiler warnings

 find_package(PkgConfig REQUIRED)
 pkg_check_modules(LIBAV REQUIRED IMPORTED_TARGET
     libavformat
     libavcodec
     libswresample
     libswscale
     libavutil
 )
 pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)

 set(CMAKE_FIND_LIBRARY_SUFFIXES .a ${CMAKE_FIND_LIBRARY_SUFFIXES})

add_compile_options(-Wall -Wextra -Wpedantic)

# Client executable
add_executable(udp_client udp_socket_client.c)

add_executable(decode h264_decode.c)
 target_link_libraries(decode PUBLIC PkgConfig::LIBAV)

# H264 visualizer (FFmpeg decode + SDL2 display)
add_executable(visualize_h264 visialize_h264.c)
target_link_libraries(visualize_h264 PUBLIC PkgConfig::LIBAV PkgConfig::SDL2)
