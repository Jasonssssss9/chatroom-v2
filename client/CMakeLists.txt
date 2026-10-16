add_library(chatroom_client_net src/net/tcp_client.cpp)
target_include_directories(chatroom_client_net PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(chatroom_client_net PUBLIC chatroom_common)
target_compile_options(chatroom_client_net PRIVATE -Wall -Wextra -Wpedantic)

add_executable(chatroom_client src/main.cpp src/console_client.cpp)
target_link_libraries(chatroom_client PRIVATE
    chatroom_client_net
    Threads::Threads
    nlohmann_json::nlohmann_json
)
target_compile_options(chatroom_client PRIVATE -Wall -Wextra -Wpedantic)
