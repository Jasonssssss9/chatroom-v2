#include "console_client.h"

#include <charconv>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {
std::uint16_t parse_port(std::string_view text) {
    unsigned int port = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), port);
    if (error != std::errc{} || end != text.data() + text.size() || port == 0 || port > 65535) {
        throw std::invalid_argument("Port must be an integer between 1 and 65535");
    }
    return static_cast<std::uint16_t>(port);
}

std::chrono::seconds parse_seconds(std::string_view text) {
    unsigned int seconds = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), seconds);
    if (error != std::errc{} || end != text.data() + text.size() || seconds == 0 || seconds > 3600) {
        throw std::invalid_argument("Heartbeat interval and timeout must be integers between 1 and 3600 seconds");
    }
    return std::chrono::seconds(seconds);
}
} // namespace

int main(int argc, char* argv[]) {
    try {
        std::vector<std::string_view> arguments;
        bool json_output = false;
        chatroom::HeartbeatConfig heartbeat;
        for (int i = 1; i < argc; ++i) {
            const std::string_view argument(argv[i]);
            if (argument == "--help") {
                std::cout << "Usage: chatroom_client [options] [IPv4 address] [TCP port] [HTTP port]\n"
                             "Defaults: 127.0.0.1, TCP 8081, HTTP 8080\n"
                             "  --json                      Display raw TCP payloads\n"
                             "  --heartbeat-interval N      Seconds between probes (default: 30)\n"
                             "  --heartbeat-timeout N       Seconds to wait for pong (default: 60)\n";
                return 0;
            }
            if (argument == "--heartbeat-interval" || argument == "--heartbeat-timeout") {
                if (++i == argc) throw std::invalid_argument("Missing heartbeat option value");
                const auto seconds = parse_seconds(argv[i]);
                if (argument == "--heartbeat-interval") heartbeat.interval = seconds;
                else heartbeat.timeout = seconds;
            } else if (argument == "--json") json_output = true;
            else arguments.push_back(argument);
        }
        if (arguments.size() > 3) throw std::invalid_argument("Use --help for client options");
        const std::string address(arguments.empty() ? "127.0.0.1" : arguments[0]);
        const auto port = arguments.size() >= 2 ? parse_port(arguments[1]) : std::uint16_t{8081};
        const auto http_port = arguments.size() >= 3 ? parse_port(arguments[2]) : std::uint16_t{8080};
        chatroom::TcpClient client(address, port);
        chatroom::AuthClient auth(address, http_port);
        chatroom::FileClient files(address, http_port);
        std::cout << "Connected to " << address << ':' << port << '\n';
        chatroom::ConsoleClient console(client, auth, files, json_output, heartbeat);
        console.run();
    } catch (const std::exception& error) {
        std::cerr << "Client error: " << error.what() << '\n';
        return 1;
    }
}
