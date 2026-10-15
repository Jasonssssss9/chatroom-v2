# List test source files explicitly as they are added.
set(TEST_SOURCES)

if(TEST_SOURCES)
    add_executable(chatroom_tests ${TEST_SOURCES})
    target_include_directories(chatroom_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/common
    )
    target_link_libraries(chatroom_tests PRIVATE
        Catch2::Catch2WithMain
        Threads::Threads
        nlohmann_json::nlohmann_json
    )
    target_compile_options(chatroom_tests PRIVATE -Wall -Wextra -Wpedantic)

    include(Catch)
    catch_discover_tests(chatroom_tests)
endif()
