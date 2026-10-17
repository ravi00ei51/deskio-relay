cmake_minimum_required(VERSION 3.22)

project(deskio_relay LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Boost REQUIRED COMPONENTS system)
find_package(Threads REQUIRED)

add_executable(deskio-relay
    src/main.cpp
)

target_link_libraries(deskio-relay
    PRIVATE
        Boost::system
        Threads::Threads
)

target_compile_options(deskio-relay
    PRIVATE
        -Wall
        -Wextra
        -Wpedantic
)
