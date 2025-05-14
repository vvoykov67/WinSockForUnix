project(WinSockForUnix)
cmake_minimum_required(VERSION 3.10)
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_BUILD_TYPE Debug)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -pedantic -Werror")

set(SOURCES
    WinSockForUnix.cpp
)

add_executable(maiWinSockForUnixn ${SOURCES} main.cpp)

message(STATUS "SHARED_DIR_PROTOCOL: ${SHARED_DIR}/include/protocol")

