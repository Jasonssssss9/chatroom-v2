#pragma once

#include <cstdlib>
#include <filesystem>
#include <stdexcept>

namespace {
struct TempDirectory {
    std::filesystem::path path;

    TempDirectory() {
        char pattern[] = "/tmp/chatroom-test-XXXXXX";
        const auto* created = ::mkdtemp(pattern);
        if (!created) throw std::runtime_error("Cannot create a test directory");
        path = created;
    }
    ~TempDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
};
} // namespace
