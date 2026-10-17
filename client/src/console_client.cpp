#include "console_client.h"

#include <cerrno>
#include <iostream>
#include <nlohmann/json.hpp>
#include <poll.h>
#include <stdexcept>
#include <sys/eventfd.h>
#include <system_error>
#include <thread>

namespace chatroom {

ConsoleClient::ConsoleClient(TcpClient& client)
    : client_(client), receiver_done_(::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)) {
    if (receiver_done_.get() < 0) {
        throw std::system_error(errno, std::generic_category(), "eventfd");
    }
}

void ConsoleClient::print(std::string_view text) {
    std::lock_guard<std::mutex> lock(output_mutex_);
    std::cout << text << std::flush;
}

bool ConsoleClient::send_line(std::string_view line) {
    if (!line.empty() && line.back() == '\r') {
        line.remove_suffix(1);
    }
    if (line == "/quit") {
        return false;
    }
    if (line.empty()) {
        return true;
    }
    if (line.size() > kMaxFrameSize) {
        print("Message exceeds the 64 KiB limit.\n> ");
    } else if (!nlohmann::json::accept(line.begin(), line.end())) {
        print("Invalid JSON. Enter a JSON request or /quit.\n> ");
    } else {
        client_.send(line);
        print("> ");
    }
    return true;
}

ConsoleClient::InputResult ConsoleClient::read_input() {
    // The receiver wakes poll on disconnect, even while the user has not entered a line.
    pollfd events[] = {{STDIN_FILENO, POLLIN, 0}, {receiver_done_.get(), POLLIN, 0}};
    std::string input;
    char buffer[4096];
    while (true) {
        if (::poll(events, 2, -1) < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw std::system_error(errno, std::generic_category(), "poll");
        }
        if (events[1].revents & POLLIN) {
            return InputResult::disconnected;
        }
        if (events[0].revents & (POLLERR | POLLNVAL)) {
            throw std::runtime_error("Standard input is unavailable");
        }
        if (!(events[0].revents & (POLLIN | POLLHUP))) {
            continue;
        }

        const auto count = ::read(STDIN_FILENO, buffer, sizeof(buffer));
        if (count < 0) {
            if (errno == EINTR || errno == EAGAIN) {
                continue;
            }
            throw std::system_error(errno, std::generic_category(), "read(stdin)");
        }
        if (count == 0) {
            return send_line(input) ? InputResult::eof : InputResult::quit;
        }
        input.append(buffer, static_cast<std::size_t>(count));
        std::size_t newline;
        while ((newline = input.find('\n')) != std::string::npos) {
            if (!send_line(std::string_view(input).substr(0, newline))) {
                return InputResult::quit;
            }
            input.erase(0, newline + 1);
        }
    }
}

void ConsoleClient::receive_messages() {
    try {
        while (auto payload = client_.receive()) {
            print("\n< " + *payload + "\n> ");
        }
    } catch (...) {
        receive_error_ = std::current_exception();
    }

    const std::uint64_t value = 1;
    ssize_t result;
    do {
        result = ::write(receiver_done_.get(), &value, sizeof(value));
    } while (result < 0 && errno == EINTR);
}

void ConsoleClient::run() {
    print("Enter one JSON request per line. Use /quit to disconnect.\n> ");
    std::thread receiver([this] { receive_messages(); });
    InputResult result;
    try {
        result = read_input();
        if (result == InputResult::eof) {
            client_.finish_sending();
        } else {
            client_.disconnect();
        }
    } catch (...) {
        client_.disconnect();
        receiver.join();
        throw;
    }
    receiver.join();
    // Local cancellation can interrupt a partially received frame.
    if (result != InputResult::quit && receive_error_) {
        std::rethrow_exception(receive_error_);
    }
    print("\nDisconnected.\n");
}

} // namespace chatroom
