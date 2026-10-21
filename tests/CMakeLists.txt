add_executable(chatroom_tests framing_test.cpp protocol_test.cpp logger_test.cpp)
target_link_libraries(chatroom_tests PRIVATE chatroom_common Catch2::Catch2WithMain)
target_compile_options(chatroom_tests PRIVATE -Wall -Wextra -Wpedantic)

if(BUILD_SERVER)
    target_sources(chatroom_tests PRIVATE
        tcp_connection_test.cpp tcp_server_test.cpp request_handler_test.cpp
        thread_pool_test.cpp config_test.cpp auth_test.cpp
    )
    target_link_libraries(chatroom_tests PRIVATE chatroom_net chatroom_app chatroom_auth)
endif()

if(BUILD_CLIENT)
    target_sources(chatroom_tests PRIVATE tcp_client_test.cpp)
    target_link_libraries(chatroom_tests PRIVATE chatroom_client_net Threads::Threads)
endif()

include(Catch)
catch_discover_tests(chatroom_tests PROPERTIES TIMEOUT 15)

if(BUILD_DATABASE_TESTS)
    add_executable(chatroom_database_tests database_test.cpp auth_http_test.cpp tcp_auth_test.cpp)
    target_link_libraries(chatroom_database_tests PRIVATE
        chatroom_http chatroom_app Catch2::Catch2WithMain)
    target_compile_options(chatroom_database_tests PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_definitions(chatroom_database_tests PRIVATE
        CHATROOM_SCHEMA_PATH="${PROJECT_SOURCE_DIR}/sql/schema.sql")
    catch_discover_tests(chatroom_database_tests
        PROPERTIES TIMEOUT 30 RUN_SERIAL TRUE LABELS database)
endif()
