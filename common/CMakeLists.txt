add_library(chatroom_common framing.cpp)
target_include_directories(chatroom_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(chatroom_common PRIVATE -Wall -Wextra -Wpedantic)
