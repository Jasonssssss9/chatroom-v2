#include "config.h"
#include "logger.h"
#include "net/tcp_server.h"
#include "request_dispatcher.h"
#include "request_handler.h"

#include <iostream>
#include <string_view>
#include <utility>
#include <vector>

int main(int argc, char* argv[]) {
    if (argc == 2 && std::string_view(argv[1]) == "--help") {
        std::cout << "Usage: chatroom_server [options]\n"
                     "  --config PATH       Load a JSON configuration file\n"
                     "  --address IPv4      Bind address (default: 127.0.0.1)\n"
                     "  --port NUMBER       TCP port, 0 selects an available port (default: 8081)\n"
                     "  --workers NUMBER    Worker thread count (default: 4)\n"
                     "  --log-level LEVEL   debug, info, warn, or error (default: info)\n"
                     "  --help              Show this help\n";
        return 0;
    }

    const char* phase = "startup";
    try {
        const std::vector<std::string_view> arguments(argv + 1, argv + argc);
        const auto config = chatroom::load_config(arguments);
        chatroom::set_log_level(config.log_level);
        chatroom::log(chatroom::LogLevel::info, "server_starting",
                      {{"address", config.address}, {"port", config.port},
                       {"worker_threads", config.worker_threads}});
        chatroom::RequestHandler handler;
        chatroom::TcpServer server(config.port, config.address);
        chatroom::RequestDispatcher dispatcher(
            server, [&handler](chatroom::ConnectionId, const chatroom::Request& request) {
                return handler.handle(request);
            }, config.worker_threads);
        server.set_message_handler([&](chatroom::ConnectionId id, std::string payload) {
            dispatcher.enqueue(id, std::move(payload));
        });
        server.set_disconnect_handler([&](chatroom::ConnectionId id) { dispatcher.disconnect(id); });
        chatroom::log(chatroom::LogLevel::info, "server_started",
                      {{"address", config.address}, {"port", server.port()},
                       {"worker_threads", config.worker_threads}});
        phase = "runtime";
        server.run();
        chatroom::log(chatroom::LogLevel::info, "server_stopped");
    } catch (const std::exception& error) {
        chatroom::log(chatroom::LogLevel::error, "server_failed",
                      {{"phase", phase}, {"error", error.what()}});
        return 1;
    }
}
