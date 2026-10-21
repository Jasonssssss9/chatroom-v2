#include "console_client.h"
#include "protocol.h"

#include <cerrno>
#include <charconv>
#include <iostream>
#include <nlohmann/json.hpp>
#include <poll.h>
#include <stdexcept>
#include <sys/eventfd.h>
#include <system_error>
#include <thread>

namespace chatroom {
namespace {
std::optional<std::string> notification_id(std::string_view payload) {
    try {
        const auto message = parse_json(payload);
        if (!message.is_object() || message.value("type", nlohmann::json{}) != "notification" ||
            message.value("action", nlohmann::json{}) != "message") return std::nullopt;
        const auto data = message.find("data");
        if (data == message.end() || !data->is_object()) return std::nullopt;
        for (const auto* field : {"sender_id", "client_message_id", "text"}) {
            const auto value = data->find(field);
            if (value == data->end() || !value->is_string()) return std::nullopt;
        }
        const auto value = data->find("message_id");
        if (value == data->end() || !value->is_string()) return std::nullopt;
        const auto& text = value->get_ref<const std::string&>();
        std::uint64_t id = 0;
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), id);
        if (parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() &&
            id != 0 && text.front() != '0') return text;
    } catch (const ProtocolError&) {
        // Keep displaying unexpected server payloads without acknowledging them.
    }
    return std::nullopt;
}
} // namespace

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
    if (!nlohmann::json::accept(line.begin(), line.end())) {
        print("Invalid JSON. Enter a JSON request or /quit.\n> ");
    } else {
        std::lock_guard<std::mutex> lock(send_mutex_);
        client_.send(line);
        print("> ");
    }
    return true;
}

ConsoleClient::InputResult ConsoleClient::read_input() {
    // The receiver wakes poll on disconnect, even while the user has not entered a line.
    pollfd events[] = {{STDIN_FILENO, POLLIN, 0}, {receiver_done_.get(), POLLIN, 0}};
    std::string input;
    bool discarding_line = false;
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
            if (discarding_line) return InputResult::eof;
            return send_line(input) ? InputResult::eof : InputResult::quit;
        }
        for (const char character : std::string_view(buffer, static_cast<std::size_t>(count))) {
            if (character == '\n') {
                if (!discarding_line && !send_line(input)) return InputResult::quit;
                input.clear();
                discarding_line = false;
            } else if (!discarding_line) {
                // Allow one trailing CR beyond the payload limit for CRLF input.
                if (input.size() < kMaxFrameSize ||
                    (input.size() == kMaxFrameSize && character == '\r')) {
                    input += character;
                } else {
                    input.clear();
                    discarding_line = true;
                    print("Message exceeds the 64 KiB limit.\n> ");
                }
            }
        }
    }
}

void ConsoleClient::display_message(std::string_view payload) {
    const auto id = notification_id(payload);
    if (!id) {
        print("\n< " + std::string(payload) + "\n> ");
        return;
    }
    if (received_messages_.insert(*id).second) {
        recent_messages_.push_back(*id);
        // A bounded, in-memory history; older duplicates can be displayed again.
        if (recent_messages_.size() > 1024) {
            received_messages_.erase(recent_messages_.front());
            recent_messages_.pop_front();
        }
        print("\n< " + std::string(payload) + "\n> ");
    }
    const auto ack = nlohmann::json{{"type", "request"}, {"request_id", "ack-" + *id},
        {"action", "ack_message"}, {"data", {{"message_id", *id}}}}.dump();
    std::lock_guard<std::mutex> lock(send_mutex_);
    if (!sending_finished_) client_.send(ack);
}

void ConsoleClient::receive_messages() {
    try {
        while (auto payload = client_.receive()) {
            display_message(*payload);
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
            std::lock_guard<std::mutex> lock(send_mutex_);
            sending_finished_ = true;
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
