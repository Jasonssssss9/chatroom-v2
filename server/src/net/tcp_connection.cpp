#include "net/tcp_connection.h"

#include <cerrno>
#include <sys/epoll.h>
#include <system_error>
#include <utility>

namespace chatroom {

TcpConnection::TcpConnection(Socket socket, std::string peer_address)
    : socket_(std::move(socket)), peer_address_(std::move(peer_address)) {}

std::vector<std::string> TcpConnection::receive() {
    std::vector<std::string> frames;
    char buffer[8192];
    while (true) {
        while (auto frame = decoder_.next()) {
            frames.push_back(std::move(*frame));
            if (frames.size() == kReceiveBatchSize) {
                return frames;
            }
        }
        const auto count = socket_.receive(buffer, sizeof(buffer));
        if (count > 0) {
            decoder_.append(std::string_view(buffer, static_cast<std::size_t>(count)));
        } else if (count == 0) {
            read_closed_ = true;
            return frames;
        } else if (errno == EINTR) {
            continue;
        } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return frames;
        } else {
            throw std::system_error(errno, std::generic_category(), "recv");
        }
    }
}

bool TcpConnection::enqueue(std::string_view frame) {
    if (output_.size() - sent_ + frame.size() > kMaxPendingBytes) {
        return false;
    }
    if (sent_ != 0) {
        output_.erase(0, sent_);
        sent_ = 0;
    }
    output_.append(frame);
    return true;
}

void TcpConnection::flush() {
    while (has_pending_output()) {
        const auto count = socket_.send(std::string_view(output_).substr(sent_));
        if (count > 0) {
            sent_ += static_cast<std::size_t>(count);
        } else if (count < 0 && errno == EINTR) {
            continue;
        } else if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return;
        } else {
            throw std::system_error(count == 0 ? EPIPE : errno, std::generic_category(), "send");
        }
    }
    output_.clear();
    sent_ = 0;
}

std::uint32_t TcpConnection::events() const noexcept {
    std::uint32_t result = EPOLLET;
    if (!read_closed_) {
        result |= EPOLLIN | EPOLLRDHUP;
    }
    if (has_pending_output()) {
        result |= EPOLLOUT;
    }
    return result;
}

} // namespace chatroom
