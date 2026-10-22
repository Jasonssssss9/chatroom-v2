#pragma once

#include "unique_fd.h"

#include <cstdint>
#include <filesystem>
#include <string_view>

namespace chatroom {

// Creates a new file exclusively and removes it unless keep() is called.
// Finish disk writes before committing the corresponding database record.
class PendingFile {
public:
    explicit PendingFile(std::filesystem::path path);
    ~PendingFile();
    PendingFile(const PendingFile&) = delete;
    PendingFile& operator=(const PendingFile&) = delete;

    void append(std::string_view bytes);
    void finish();
    void keep() noexcept { kept_ = true; }
    std::uint64_t size() const { return size_; }

private:
    std::filesystem::path path_;
    UniqueFd fd_;
    std::uint64_t size_ = 0;
    bool kept_ = false;
};

} // namespace chatroom
