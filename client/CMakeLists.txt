# List client source files explicitly as they are added.
set(CLIENT_SOURCES
    # main.cpp
)

if(CLIENT_SOURCES)
    add_executable(chatroom_client ${CLIENT_SOURCES})
    target_include_directories(chatroom_client PRIVATE
        ${PROJECT_SOURCE_DIR}/common
    )
    target_link_libraries(chatroom_client PRIVATE
        Threads::Threads
        nlohmann_json::nlohmann_json
    )
    target_compile_options(chatroom_client PRIVATE -Wall -Wextra -Wpedantic)
endif()
