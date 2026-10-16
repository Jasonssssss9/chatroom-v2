# List server source files explicitly as they are added.
set(SERVER_SOURCES
    # src/main.cpp
)

if(SERVER_SOURCES)
    add_executable(chatroom_server ${SERVER_SOURCES})
    target_include_directories(chatroom_server PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${PROJECT_SOURCE_DIR}/common
    )
    target_link_libraries(chatroom_server PRIVATE
        Threads::Threads
        nlohmann_json::nlohmann_json
    )
    target_compile_options(chatroom_server PRIVATE -Wall -Wextra -Wpedantic)
endif()
