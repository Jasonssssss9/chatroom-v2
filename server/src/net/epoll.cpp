#include "net/epoll.h"

#include <cerrno>
#include <system_error>

namespace chatroom {

Epoll::Epoll() : fd_(::epoll_create1(EPOLL_CLOEXEC)) {
    if (fd_.get() < 0) {
        throw std::system_error(errno, std::generic_category(), "epoll_create1");
    }
}

void Epoll::control(int operation, int fd, std::uint64_t token, std::uint32_t events) {
    epoll_event event{};
    event.data.u64 = token;
    event.events = events;
    if (::epoll_ctl(fd_.get(), operation, fd, &event) < 0) {
        throw std::system_error(errno, std::generic_category(), "epoll_ctl");
    }
}

void Epoll::add(int fd, std::uint64_t token, std::uint32_t events) {
    control(EPOLL_CTL_ADD, fd, token, events);
}

void Epoll::modify(int fd, std::uint64_t token, std::uint32_t events) {
    control(EPOLL_CTL_MOD, fd, token, events);
}

void Epoll::remove(int fd) {
    if (::epoll_ctl(fd_.get(), EPOLL_CTL_DEL, fd, nullptr) < 0) {
        throw std::system_error(errno, std::generic_category(), "epoll_ctl(DEL)");
    }
}

int Epoll::wait(epoll_event* events, int capacity, int timeout_ms) {
    const int count = ::epoll_wait(fd_.get(), events, capacity, timeout_ms);
    if (count < 0) {
        if (errno == EINTR) {
            return 0;
        }
        throw std::system_error(errno, std::generic_category(), "epoll_wait");
    }
    return count;
}

} // namespace chatroom
