#include "framing.h"

#include <arpa/inet.h>
#include <cstdint>
#include <cstring>
#include <stdexcept>

namespace chatroom {

std::string encode_frame(std::string_view payload) {
    if (payload.size() > kMaxFrameSize) {
        throw std::length_error("Frame exceeds the 64 KiB limit");
    }

    const auto length = htonl(static_cast<std::uint32_t>(payload.size()));
    std::string frame(reinterpret_cast<const char*>(&length), sizeof(length));
    frame.append(payload);
    return frame;
}

void FrameDecoder::append(std::string_view bytes) {
    if (offset_ != 0) {
        buffer_.erase(0, offset_);
        offset_ = 0;
    }
    buffer_.append(bytes);
}

std::optional<std::string> FrameDecoder::next() {
    const auto available = buffer_.size() - offset_;
    if (available < sizeof(std::uint32_t)) {
        return std::nullopt;
    }

    std::uint32_t length;
    std::memcpy(&length, buffer_.data() + offset_, sizeof(length));
    length = ntohl(length);
    if (length > kMaxFrameSize) {
        throw std::length_error("Frame exceeds the 64 KiB limit");
    }
    if (available - sizeof(length) < length) {
        return std::nullopt;
    }

    std::string payload = buffer_.substr(offset_ + sizeof(length), length);
    offset_ += sizeof(length) + length;
    if (offset_ == buffer_.size()) {
        buffer_.clear();
        offset_ = 0;
    }
    return payload;
}

} // namespace chatroom
