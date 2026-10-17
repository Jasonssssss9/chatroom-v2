#include "net/tcp_server.h"
#include "logger.h"

#include <array>
#include <cerrno>
#include <stdexcept>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <system_error>
#include <utility>

namespace chatroom {
namespace {
constexpr std::uint64_t kListener = 0;
constexpr std::uint64_t kWakeup = 1;
} // namespace

TcpServer::TcpServer(std::uint16_t port, const std::string& address)
    : listener_(Socket::listen_on(address, port)),
      wakeup_fd_(::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)) {
    if (wakeup_fd_.get() < 0) {
        throw std::system_error(errno, std::generic_category(), "eventfd");
    }
    epoll_.add(listener_.fd(), kListener, EPOLLIN | EPOLLET);
    epoll_.add(wakeup_fd_.get(), kWakeup, EPOLLIN | EPOLLET);
}

void TcpServer::set_message_handler(MessageHandler handler) {
    on_message_ = std::move(handler);
}

void TcpServer::set_disconnect_handler(DisconnectHandler handler) {
    on_disconnect_ = std::move(handler);
}

void TcpServer::notify() {
    const std::uint64_t value = 1;
    ssize_t result;
    do {
        result = ::write(wakeup_fd_.get(), &value, sizeof(value));
    } while (result < 0 && errno == EINTR);
    // EAGAIN means the counter is full, so the loop is already scheduled to wake.
    if (result < 0 && errno != EAGAIN) {
        throw std::system_error(errno, std::generic_category(), "write(eventfd)");
    }
}

void TcpServer::send(ConnectionId id, std::string_view payload) {
    PendingSend pending{id, encode_frame(payload)};
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        if (stopped_) {
            return;
        }
        pending_sends_.push(std::move(pending));
    }
    notify();
}

void TcpServer::post(std::function<void()> callback) {
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        if (stopped_) {
            return;
        }
        pending_callbacks_.push(std::move(callback));
    }
    notify();
}

void TcpServer::process_callbacks() {
    std::queue<std::function<void()>> callbacks;
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        callbacks.swap(pending_callbacks_);
    }
    while (!callbacks.empty() && !stopped_) {
        callbacks.front()();
        callbacks.pop();
    }
}

void TcpServer::defer_close(ConnectionId id) {
    const auto it = connections_.find(id);
    if (it != connections_.end()) {
        it->second.set_close_deferred(true);
    }
}

void TcpServer::resume_close(ConnectionId id) {
    const auto it = connections_.find(id);
    if (it == connections_.end()) {
        return;
    }
    it->second.set_close_deferred(false);
    process_sends();
    update_connection(id);
}

void TcpServer::stop() {
    stopped_ = true;
    notify();
}

void TcpServer::accept_connections() {
    while (true) {
        std::string peer;
        auto socket = listener_.accept(peer);
        if (socket.fd() < 0) {
            if (errno == EINTR || errno == ECONNABORTED) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return;
            }
            throw std::system_error(errno, std::generic_category(), "accept4");
        }

        const auto id = next_id_++;
        auto it = connections_.try_emplace(id, std::move(socket), std::move(peer)).first;
        try {
            epoll_.add(it->second.fd(), id, it->second.events());
        } catch (...) {
            connections_.erase(it);
            throw;
        }
        log(LogLevel::info, "connection_opened",
            {{"connection_id", id}, {"peer", it->second.peer_address()}});
    }
}

void TcpServer::process_sends() {
    std::queue<PendingSend> sends;
    std::vector<ConnectionId> updated;
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        sends.swap(pending_sends_);
    }
    while (!sends.empty()) {
        auto& pending = sends.front();
        auto it = connections_.find(pending.id);
        if (it != connections_.end()) {
            try {
                if (!it->second.enqueue(pending.frame)) {
                    throw std::length_error("Connection output exceeds the 1 MiB limit");
                }
                it->second.flush();
                updated.push_back(pending.id);
            } catch (const std::exception& error) {
                log(LogLevel::warn, "connection_error",
                    {{"connection_id", pending.id}, {"error", error.what()}});
                close_connection(pending.id, "error");
            }
        }
        sends.pop();
    }
    // Finish the batch before closing half-closed peers with several queued replies.
    for (const auto id : updated) {
        try {
            update_connection(id);
        } catch (const std::exception& error) {
            log(LogLevel::warn, "connection_error",
                {{"connection_id", id}, {"error", error.what()}});
            close_connection(id, "error");
        }
    }
}

void TcpServer::handle_event(ConnectionId id, std::uint32_t events) {
    auto it = connections_.find(id);
    if (it == connections_.end()) {
        return;
    }
    auto& connection = it->second;
    if (events & EPOLLERR) {
        int error = 0;
        socklen_t size = sizeof(error);
        if (::getsockopt(connection.fd(), SOL_SOCKET, SO_ERROR, &error, &size) < 0) {
            throw std::system_error(errno, std::generic_category(), "getsockopt(SO_ERROR)");
        }
        throw std::system_error(error == 0 ? EIO : error, std::generic_category(), "socket error");
    }
    if (events & (EPOLLIN | EPOLLRDHUP | EPOLLHUP)) {
        auto frames = connection.receive();
        for (auto& frame : frames) {
            if (on_message_) {
                on_message_(id, std::move(frame));
            }
        }
        // Apply synchronous replies before closing a peer that sent its final data and FIN.
        process_sends();
    }

    // Sending can remove a connection, so look it up again before accessing it.
    it = connections_.find(id);
    if (it == connections_.end()) {
        return;
    }
    if (events & EPOLLOUT) {
        it->second.flush();
    }
    update_connection(id);
}

void TcpServer::update_connection(ConnectionId id) {
    auto it = connections_.find(id);
    if (it == connections_.end()) {
        return;
    }
    if (it->second.read_closed() && !it->second.close_deferred() &&
        !it->second.has_pending_output()) {
        close_connection(id, "peer_closed");
        return;
    }
    epoll_.modify(it->second.fd(), id, it->second.events());
}

void TcpServer::close_connection(ConnectionId id, std::string_view reason) {
    // Extraction keeps the socket owned even if removing it from epoll fails.
    auto connection = connections_.extract(id);
    if (connection.empty()) {
        return;
    }
    epoll_.remove(connection.mapped().fd());
    log(LogLevel::info, "connection_closed",
        {{"connection_id", id}, {"peer", connection.mapped().peer_address()}, {"reason", reason}});
    if (on_disconnect_) {
        on_disconnect_(id);
    }
}

void TcpServer::run() {
    std::array<epoll_event, 64> events;
    try {
        while (!stopped_) {
            const int count = epoll_.wait(events.data(), static_cast<int>(events.size()));
            for (int i = 0; i < count && !stopped_; ++i) {
                const auto id = events[i].data.u64;
                if (id == kListener) {
                    accept_connections();
                } else if (id == kWakeup) {
                    std::uint64_t value;
                    ssize_t result;
                    do {
                        result = ::read(wakeup_fd_.get(), &value, sizeof(value));
                    } while (result < 0 && errno == EINTR);
                    if (result < 0 && errno != EAGAIN) {
                        throw std::system_error(errno, std::generic_category(), "read(eventfd)");
                    }
                    process_callbacks();
                    if (!stopped_) {
                        process_sends();
                    }
                } else {
                    try {
                        handle_event(id, events[i].events);
                    } catch (const std::exception& error) {
                        log(LogLevel::warn, "connection_error",
                            {{"connection_id", id}, {"error", error.what()}});
                        close_connection(id, "error");
                    }
                }
            }
        }
    } catch (...) {
        cleanup();
        throw;
    }
    cleanup();
}

void TcpServer::cleanup() {
    stopped_ = true;
    while (!connections_.empty()) {
        close_connection(connections_.begin()->first, "server_stopped");
    }
    std::lock_guard<std::mutex> lock(queue_mutex_);
    pending_sends_ = {};
    pending_callbacks_ = {};
}

} // namespace chatroom
