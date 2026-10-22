#include "net/tcp_server.h"
#include "logger.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <stdexcept>
#include <sys/eventfd.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <system_error>
#include <utility>

namespace chatroom {
namespace {
constexpr std::uint64_t kListener = 0;
constexpr std::uint64_t kWakeup = 1;
constexpr std::uint64_t kShutdownSignal = 2;
constexpr auto kAcceptRetryDelay = std::chrono::milliseconds(200);

bool retry_accept_error(int error) {
    // Linux can report a pending connection's network error through accept4().
    switch (error) {
    case EINTR:
    case ECONNABORTED:
    case ENETDOWN:
    case EPROTO:
    case ENOPROTOOPT:
    case EHOSTDOWN:
    case ENONET:
    case EHOSTUNREACH:
    case EOPNOTSUPP:
    case ENETUNREACH:
        return true;
    default:
        return false;
    }
}
} // namespace

TcpServer::TcpServer(std::uint16_t port, const std::string& address, std::size_t max_connections,
                     std::chrono::milliseconds idle_timeout)
    : listener_(Socket::listen_on(address, port)),
      wakeup_fd_(::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)),
      max_connections_(max_connections), idle_timeout_(idle_timeout) {
    if (max_connections_ == 0) {
        throw std::invalid_argument("Maximum connections must be positive");
    }
    if (idle_timeout_ <= std::chrono::milliseconds::zero() || idle_timeout_ > std::chrono::hours(24)) {
        throw std::invalid_argument("Idle timeout must be positive and at most 24 hours");
    }
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
            const int error = errno;
            if (retry_accept_error(error)) {
                continue;
            }
            if (error == EAGAIN || error == EWOULDBLOCK) {
                return;
            }
            if (error == EMFILE || error == ENFILE || error == ENOBUFS || error == ENOMEM) {
                epoll_.remove(listener_.fd());
                accept_retry_at_ = std::chrono::steady_clock::now() + kAcceptRetryDelay;
                log(LogLevel::warn, "accept_paused",
                    {{"errno", error}, {"error", std::generic_category().message(error)},
                     {"retry_ms", kAcceptRetryDelay.count()}});
                return;
            }
            throw std::system_error(error, std::generic_category(), "accept4");
        }
        if (connections_.size() >= max_connections_) {
            log(LogLevel::warn, "connection_rejected",
                {{"peer", peer}, {"reason", "connection_limit"},
                 {"max_connections", max_connections_}});
            continue;
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
    if (events & EPOLLERR) {
        int error = 0;
        socklen_t size = sizeof(error);
        if (::getsockopt(it->second.fd(), SOL_SOCKET, SO_ERROR, &error, &size) < 0) {
            throw std::system_error(errno, std::generic_category(), "getsockopt(SO_ERROR)");
        }
        throw std::system_error(error == 0 ? EIO : error, std::generic_category(), "socket error");
    }
    if (!it->second.read_closed() && (events & (EPOLLIN | EPOLLRDHUP | EPOLLHUP))) {
        while (true) {
            auto frames = it->second.receive();
            for (auto& frame : frames) {
                if (on_message_) {
                    on_message_(id, std::move(frame));
                }
                // A request limit can close the connection from the message callback.
                it = connections_.find(id);
                if (it == connections_.end()) return;
            }
            if (frames.size() < TcpConnection::kReceiveBatchSize) break;
        }
        if (it->second.truncated_frame()) {
            log(LogLevel::warn, "connection_error",
                {{"connection_id", id}, {"peer", it->second.peer_address()},
                 {"code", "truncated_frame"}, {"error", "Connection closed in the middle of a frame"}});
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
        close_connection(id, it->second.truncated_frame() ? "truncated_frame" : "peer_closed");
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

void TcpServer::close_idle_connections() {
    const auto now = std::chrono::steady_clock::now();
    std::vector<ConnectionId> expired;
    for (const auto& [id, connection] : connections_) {
        if (now - connection.last_activity() >= idle_timeout_) expired.push_back(id);
    }
    for (const auto id : expired) close_connection(id, "idle_timeout");
}

void TcpServer::run(int shutdown_signal_fd) {
    std::array<epoll_event, 64> events;
    // A small periodic scan shares epoll's existing timeout with accept retries.
    const auto scan_interval = std::min(idle_timeout_, std::chrono::milliseconds(1000));
    auto next_idle_check = std::chrono::steady_clock::now() + scan_interval;
    try {
        if (shutdown_signal_fd >= 0) {
            epoll_.add(shutdown_signal_fd, kShutdownSignal, EPOLLIN);
        }
        while (!stopped_) {
            if (std::chrono::steady_clock::now() >= next_idle_check) {
                close_idle_connections();
                next_idle_check = std::chrono::steady_clock::now() + scan_interval;
            }
            if (accept_retry_at_ && std::chrono::steady_clock::now() >= *accept_retry_at_) {
                epoll_.add(listener_.fd(), kListener, EPOLLIN | EPOLLET);
                accept_retry_at_.reset();
                log(LogLevel::debug, "accept_retry");
                // Retry explicitly: the pending connection may have no new ET notification.
                accept_connections();
            }
            const auto deadline = accept_retry_at_ ? std::min(next_idle_check, *accept_retry_at_) : next_idle_check;
            const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
            const int timeout_ms = std::max(0, static_cast<int>(remaining.count()));
            const int count = epoll_.wait(events.data(), static_cast<int>(events.size()), timeout_ms);
            for (int i = 0; i < count && !stopped_; ++i) {
                const auto id = events[i].data.u64;
                if (id == kListener) {
                    if (!accept_retry_at_) accept_connections();
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
                } else if (id == kShutdownSignal) {
                    signalfd_siginfo signal{};
                    ssize_t result;
                    do {
                        result = ::read(shutdown_signal_fd, &signal, sizeof(signal));
                    } while (result < 0 && errno == EINTR);
                    if (result < 0 && errno == EAGAIN) continue;
                    if (result != static_cast<ssize_t>(sizeof(signal))) {
                        throw std::system_error(result < 0 ? errno : EIO,
                                                std::generic_category(), "read(signalfd)");
                    }
                    log(LogLevel::info, "shutdown_requested", {{"signal", signal.ssi_signo}});
                    stop();
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
