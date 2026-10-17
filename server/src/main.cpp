#include "net/tcp_server.h"
#include "request_handler.h"

#include <charconv>
#include <iostream>
#include <string_view>

int main(int argc, char* argv[]) {
    unsigned int port = 8081;
    if (argc > 3) {
        std::cerr << "Usage: chatroom_server [port] [IPv4 bind address]\n";
        return 1;
    }
    if (argc >= 2) {
        const std::string_view text(argv[1]);
        const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), port);
        if (error != std::errc{} || end != text.data() + text.size() || port > 65535) {
            std::cerr << "Port must be an integer between 0 and 65535\n";
            return 1;
        }
    }

    try {
        const std::string address = argc == 3 ? argv[2] : "127.0.0.1";
        chatroom::RequestHandler handler;
        chatroom::TcpServer server(static_cast<std::uint16_t>(port), address);
        server.set_message_handler([&](chatroom::ConnectionId id, std::string payload) {
            server.send(id, handler.handle(payload).dump());
        });
        std::cout << "Chat server listening on " << address << ':' << server.port()
                  << std::endl;
        server.run();
    } catch (const std::exception& error) {
        std::cerr << "Server error: " << error.what() << '\n';
        return 1;
    }
}
