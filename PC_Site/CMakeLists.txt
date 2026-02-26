cmake_minimum_required(VERSION 3.10)
project(SocketDemo C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Compiler warnings
add_compile_options(-Wall -Wextra -Wpedantic)

# Client executable
add_executable(udp_client udp_socket_client.c)
