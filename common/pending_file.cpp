#include "pending_file.h"

#include <cerrno>
#include <fcntl.h>
#include <system_error>

namespace chatroom {

PendingFile::PendingFile(std::filesystem::path path)
    : path_(std::move(path)), fd_(::open(path_.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600)) {
    if (fd_.get() < 0) throw std::system_error(errno, std::generic_category(), "Cannot create file");
}

PendingFile::~PendingFile() {
    if (!kept_) {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
    }
}

void PendingFile::append(std::string_view bytes) {
    while (!bytes.empty()) {
        const auto written = ::write(fd_.get(), bytes.data(), bytes.size());
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) throw std::system_error(written < 0 ? errno : EIO,
                                                 std::generic_category(), "Cannot write file");
        size_ += static_cast<std::uint64_t>(written);
        bytes.remove_prefix(static_cast<std::size_t>(written));
    }
}

void PendingFile::finish() {
    int result;
    do { result = ::fsync(fd_.get()); } while (result < 0 && errno == EINTR);
    if (result < 0) throw std::system_error(errno, std::generic_category(), "Cannot finish file");
    fd_.reset();
}

} // namespace chatroom
