cmake_minimum_required(VERSION 3.20)
project(chatroom-server-v2 VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(BUILD_SERVER "Build the chat server" ON)
option(BUILD_CLIENT "Build the command-line client" ON)
option(BUILD_DATABASE_TESTS "Build MySQL integration tests (requires chatroom_test)" OFF)

find_package(Threads REQUIRED)
include(CTest)
if(BUILD_DATABASE_TESTS AND (NOT BUILD_SERVER OR NOT BUILD_TESTING))
    message(FATAL_ERROR "BUILD_DATABASE_TESTS requires BUILD_SERVER and BUILD_TESTING")
endif()
include(cmake/Dependencies.cmake)

add_subdirectory(common)

if(BUILD_SERVER)
    add_subdirectory(server)
endif()

if(BUILD_CLIENT)
    add_subdirectory(client)
endif()

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
