#pragma once

#include "unique_fd.h"

#include <cstdint>
#include <sys/epoll.h>

namespace chatroom {

// Owns an epoll instance. Tokens identify connections independently of fd reuse.
class Epoll {
public:
    Epoll();

    void add(int fd, std::uint64_t token, std::uint32_t events);
    void modify(int fd, std::uint64_t token, std::uint32_t events);
    void remove(int fd);

    // Blocks until events arrive; an interrupted wait returns zero events.
    int wait(epoll_event* events, int capacity);

private:
    void control(int operation, int fd, std::uint64_t token, std::uint32_t events);
    UniqueFd fd_;
};

} // namespace chatroom
