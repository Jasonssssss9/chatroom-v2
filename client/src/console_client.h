#pragma once

#include "net/tcp_client.h"

#include <exception>
#include <deque>
#include <mutex>
#include <unordered_set>
#include <string_view>

namespace chatroom {

// Runs terminal input on the calling thread and replies on a joined receiver thread.
class ConsoleClient {
public:
    explicit ConsoleClient(TcpClient& client);
    void run();

private:
    enum class InputResult { quit, eof, disconnected };

    InputResult read_input();
    bool send_line(std::string_view line);
    void receive_messages();
    void display_message(std::string_view payload);
    void print(std::string_view text);

    TcpClient& client_;
    UniqueFd receiver_done_;
    std::mutex output_mutex_;
    // Serialize terminal requests, automatic ACKs, and the stdin EOF half-close.
    std::mutex send_mutex_;
    bool sending_finished_ = false;
    std::unordered_set<std::string> received_messages_;
    std::deque<std::string> recent_messages_;
    std::exception_ptr receive_error_;
};

} // namespace chatroom
