cmake_minimum_required(VERSION 3.10)
project(SocketDemo C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Client executable
add_executable(udp_client udp_socket_client.c)
add_executable(udp_server udp_socket_server.c)
