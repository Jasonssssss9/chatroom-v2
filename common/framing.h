#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace chatroom {

inline constexpr std::size_t kMaxFrameSize = 64 * 1024;

// Prefixes a payload with its four-byte length in network byte order.
// Throws std::length_error if the payload exceeds kMaxFrameSize.
std::string encode_frame(std::string_view payload);

// Retains incomplete input and extracts complete frames from a TCP byte stream.
// Payloads are bytes; JSON parsing belongs to the application.
class FrameDecoder {
public:
    void append(std::string_view bytes);

    // Returns no value until a frame is complete. Rejects oversized lengths.
    std::optional<std::string> next();
    bool empty() const noexcept { return buffer_.size() == offset_; }

private:
    std::string buffer_;
    std::size_t offset_ = 0;
};

} // namespace chatroom
