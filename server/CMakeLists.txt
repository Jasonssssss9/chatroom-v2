add_library(chatroom_net
    src/net/socket.cpp
    src/net/epoll.cpp
    src/net/tcp_connection.cpp
    src/net/tcp_server.cpp
)
target_include_directories(chatroom_net PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(chatroom_net PUBLIC chatroom_common Threads::Threads)
target_compile_definitions(chatroom_net PRIVATE _GNU_SOURCE)
target_compile_options(chatroom_net PRIVATE -Wall -Wextra -Wpedantic)

add_library(chatroom_app src/request_handler.cpp)
target_include_directories(chatroom_app PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(chatroom_app PUBLIC chatroom_common)
target_compile_options(chatroom_app PRIVATE -Wall -Wextra -Wpedantic)

add_executable(chatroom_server src/main.cpp)
target_link_libraries(chatroom_server PRIVATE chatroom_net chatroom_app)
target_compile_options(chatroom_server PRIVATE -Wall -Wextra -Wpedantic)
