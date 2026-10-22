add_library(chatroom_common framing.cpp protocol.cpp logger.cpp pending_file.cpp)
target_include_directories(chatroom_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(chatroom_common PUBLIC nlohmann_json::nlohmann_json Threads::Threads)
target_compile_options(chatroom_common PRIVATE -Wall -Wextra -Wpedantic)
