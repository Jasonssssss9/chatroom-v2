#pragma once

#include "net/tcp_client.h"

#include <exception>
#include <mutex>
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
    void print(std::string_view text);

    TcpClient& client_;
    UniqueFd receiver_done_;
    std::mutex output_mutex_;
    std::exception_ptr receive_error_;
};

} // namespace chatroom
