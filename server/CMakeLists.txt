find_package(PkgConfig REQUIRED)
pkg_check_modules(MYSQL REQUIRED IMPORTED_TARGET mysqlclient)
find_package(OpenSSL 3.0 REQUIRED)

set(JWT_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(JWT_DISABLE_PICOJSON ON CACHE BOOL "" FORCE)
FetchContent_Declare(jwt_cpp
    GIT_REPOSITORY https://github.com/Thalhammer/jwt-cpp.git
    GIT_TAG v0.7.2
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(jwt_cpp)

add_library(chatroom_db src/db/database.cpp src/db/chat_store.cpp)
target_include_directories(chatroom_db PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(chatroom_db PUBLIC chatroom_common PkgConfig::MYSQL Threads::Threads)
target_compile_options(chatroom_db PRIVATE -Wall -Wextra -Wpedantic)

add_library(chatroom_auth src/auth/password.cpp src/auth/token_service.cpp src/auth/auth_service.cpp)
target_include_directories(chatroom_auth PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(chatroom_auth PUBLIC chatroom_db jwt-cpp::jwt-cpp OpenSSL::Crypto)
target_compile_options(chatroom_auth PRIVATE -Wall -Wextra -Wpedantic)

add_library(chatroom_http src/http/http_server.cpp)
target_include_directories(chatroom_http PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(chatroom_http PUBLIC chatroom_auth httplib::httplib)
target_compile_options(chatroom_http PRIVATE -Wall -Wextra -Wpedantic)

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
    src/session_manager.cpp
    src/thread_pool.cpp
)
target_include_directories(chatroom_app PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(chatroom_app PUBLIC chatroom_net chatroom_auth)
target_compile_options(chatroom_app PRIVATE -Wall -Wextra -Wpedantic)

add_executable(chatroom_server src/main.cpp)
target_link_libraries(chatroom_server PRIVATE chatroom_app chatroom_http)
target_compile_options(chatroom_server PRIVATE -Wall -Wextra -Wpedantic)
