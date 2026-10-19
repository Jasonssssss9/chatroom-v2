find_package(PkgConfig REQUIRED)
pkg_check_modules(MYSQL REQUIRED IMPORTED_TARGET mysqlclient)

add_library(chatroom_db src/db/database.cpp src/db/chat_store.cpp)
target_include_directories(chatroom_db PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(chatroom_db PUBLIC chatroom_common PkgConfig::MYSQL Threads::Threads)
target_compile_options(chatroom_db PRIVATE -Wall -Wextra -Wpedantic)

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

add_library(chatroom_app
    src/config.cpp
    src/request_handler.cpp
    src/request_dispatcher.cpp
    src/thread_pool.cpp
)
target_include_directories(chatroom_app PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(chatroom_app PUBLIC chatroom_net)
target_compile_options(chatroom_app PRIVATE -Wall -Wextra -Wpedantic)

add_executable(chatroom_server src/main.cpp)
target_link_libraries(chatroom_server PRIVATE chatroom_app chatroom_db)
target_compile_options(chatroom_server PRIVATE -Wall -Wextra -Wpedantic)
