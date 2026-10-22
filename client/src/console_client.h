#pragma once

#include "net/tcp_client.h"
#include "auth_client.h"
#include "file_client.h"

#include <deque>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <string_view>
#include <unordered_set>
#include <unordered_map>

namespace chatroom {

struct HeartbeatConfig {
    std::chrono::seconds interval{30};
    std::chrono::seconds timeout{60};
};

// Terminal input, a receiver, and a timer share one serialized TCP sender.
class ConsoleClient {
public:
    ConsoleClient(TcpClient& client, AuthClient& auth, FileClient& files, bool json_output = false,
                  HeartbeatConfig heartbeat = {});
    void run();

private:
    enum class InputResult { quit, eof, disconnected };

    InputResult read_input();
    bool send_line(std::string_view line);
    bool handle_command(std::string_view line);
    void send_request(const std::string& action, nlohmann::json data = nlohmann::json::object());
    void receive_messages();
    // Waits for one matching pong at a time and reports the session's expiry once.
    void heartbeat_loop();
    void stop_heartbeat();
    void display_message(std::string_view payload);
    void print(std::string_view text);

    TcpClient& client_;
    AuthClient& auth_;
    FileClient& files_;
    bool json_output_;
    HeartbeatConfig heartbeat_;
    std::string message_prefix_;
    std::uint64_t next_request_id_ = 1;
    UniqueFd receiver_done_;
    std::mutex output_mutex_;
    // Serialize terminal requests, automatic ACKs, heartbeats, and the EOF half-close.
    std::mutex send_mutex_;
    bool sending_finished_ = false;
    // Connection state is shared with the receiver and timer, independently of socket writes.
    std::mutex state_mutex_;
    std::condition_variable state_changed_;
    bool stopping_ = false;
    std::string heartbeat_request_;
    std::optional<std::chrono::steady_clock::time_point> heartbeat_deadline_;
    std::chrono::steady_clock::time_point next_heartbeat_;
    std::optional<std::chrono::system_clock::time_point> session_expires_at_;
    // Protected by state_mutex_; retain tokens only after TCP authentication succeeds.
    std::unordered_map<std::string, std::string> pending_tokens_;
    std::string token_;
    std::unordered_set<std::string> received_messages_;
    std::deque<std::string> recent_messages_;
    std::exception_ptr receive_error_;
    std::exception_ptr heartbeat_error_;
};

} // namespace chatroom
