#pragma once

#include "net/tcp_client.h"
#include "auth_client.h"

#include <deque>
#include <exception>
#include <mutex>
#include <string_view>
#include <unordered_set>

namespace chatroom {

// Runs terminal input on the calling thread and replies on a joined receiver thread.
class ConsoleClient {
public:
    ConsoleClient(TcpClient& client, AuthClient& auth, bool json_output = false);
    void run();

private:
    enum class InputResult { quit, eof, disconnected };

    InputResult read_input();
    bool send_line(std::string_view line);
    bool handle_command(std::string_view line);
    void send_request(const std::string& action, nlohmann::json data = nlohmann::json::object());
    void receive_messages();
    void display_message(std::string_view payload);
    void print(std::string_view text);

    TcpClient& client_;
    AuthClient& auth_;
    bool json_output_;
    std::string message_prefix_;
    std::uint64_t next_request_id_ = 1;
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
