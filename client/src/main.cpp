#include "console_client.h"

#include <charconv>
#include <iostream>
#include <string_view>

int main(int argc, char* argv[]) {
    if (argc > 3) {
        std::cerr << "Usage: chatroom_client [IPv4 server address] [port]\n";
        return 1;
    }
    unsigned int port = 8081;
    if (argc == 3) {
        const std::string_view text(argv[2]);
        const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), port);
        if (error != std::errc{} || end != text.data() + text.size() || port == 0 || port > 65535) {
            std::cerr << "Port must be an integer between 1 and 65535\n";
            return 1;
        }
    }

    try {
        const std::string address = argc >= 2 ? argv[1] : "127.0.0.1";
        chatroom::TcpClient client(address, static_cast<std::uint16_t>(port));
        std::cout << "Connected to " << address << ':' << port << '\n';
        chatroom::ConsoleClient console(client);
        console.run();
    } catch (const std::exception& error) {
        std::cerr << "Client error: " << error.what() << '\n';
        return 1;
    }
}
