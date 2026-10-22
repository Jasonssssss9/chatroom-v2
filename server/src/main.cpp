#include "config.h"
#include "auth/auth_service.h"
#include "db/database.h"
#include "http/http_server.h"
#include "logger.h"
#include "net/tcp_server.h"
#include "request_dispatcher.h"
#include "request_handler.h"

#include <cerrno>
#include <csignal>
#include <iostream>
#include <pthread.h>
#include <string_view>
#include <sys/signalfd.h>
#include <system_error>
#include <utility>
#include <vector>

namespace {
chatroom::UniqueFd make_shutdown_signal_fd() {
    sigset_t signals;
    ::sigemptyset(&signals);
    ::sigaddset(&signals, SIGINT);
    ::sigaddset(&signals, SIGTERM);
    // Workers inherit this mask. Keep it blocked through teardown as well.
    const int error = ::pthread_sigmask(SIG_BLOCK, &signals, nullptr);
    if (error != 0) throw std::system_error(error, std::generic_category(), "pthread_sigmask");
    chatroom::UniqueFd fd(::signalfd(-1, &signals, SFD_NONBLOCK | SFD_CLOEXEC));
    if (fd.get() < 0) throw std::system_error(errno, std::generic_category(), "signalfd");
    return fd;
}
} // namespace

int main(int argc, char* argv[]) {
    if (argc == 2 && std::string_view(argv[1]) == "--help") {
        std::cout << "Usage: chatroom_server [options]\n"
                     "  --config PATH       Load a JSON configuration file\n"
                     "  --address IPv4      Bind address (default: 127.0.0.1)\n"
                     "  --port NUMBER       TCP port, 0 selects an available port (default: 8081)\n"
                     "  --workers NUMBER    Worker thread count (default: 4)\n"
                     "  --http-port NUMBER  HTTP port (default: 8080; 0 selects an available port)\n"
                     "  --http-workers N    HTTP worker thread count (default: 4)\n"
                     "  --max-connections N Maximum active connections (default: 1024)\n"
                     "  --idle-timeout N    Idle TCP timeout in seconds (default: 90)\n"
                     "  --max-requests-per-connection N  Unfinished requests per client (default: 32)\n"
                     "  --max-pending-requests N         Unfinished requests in total (default: 1024)\n"
                     "  --log-level LEVEL   debug, info, warn, or error (default: info)\n"
                     "  --help              Show this help\n";
        return 0;
    }

    const char* phase = "startup";
    try {
        const std::vector<std::string_view> arguments(argv + 1, argv + argc);
        const auto config = chatroom::load_config(arguments);
        chatroom::set_log_level(config.log_level);
        const auto shutdown_signal = make_shutdown_signal_fd();
        chatroom::log(chatroom::LogLevel::info, "server_starting",
                      {{"address", config.address}, {"port", config.port},
                       {"worker_threads", config.worker_threads},
                       {"http_port", config.http.port}, {"http_workers", config.http.worker_threads},
                       {"max_connections", config.max_connections},
                       {"idle_timeout_seconds", config.idle_timeout_seconds},
                       {"max_requests_per_connection", config.max_requests_per_connection},
                       {"max_pending_requests", config.max_pending_requests}});
        chatroom::initialize_mysql();
        {
            chatroom::Database database(config.database);
        }
        chatroom::log(chatroom::LogLevel::info, "database_connected",
                      {{"host", config.database.host}, {"port", config.database.port},
                       {"database", config.database.name}});
        {
            chatroom::TokenService tokens(config.jwt_secret, config.token_lifetime_seconds);
            chatroom::AuthService auth(tokens);
            chatroom::RequestHandler handler(tokens, config.database);
            chatroom::TcpServer server(config.port, config.address, config.max_connections,
                                       std::chrono::seconds(config.idle_timeout_seconds));
            chatroom::RequestDispatcher dispatcher(
                server, [&handler](chatroom::ConnectionId, const chatroom::Request& request,
                                   std::optional<chatroom::Session> session) {
                    return handler.handle(request, session);
                }, config.worker_threads, config.max_requests_per_connection, config.max_pending_requests,
                [&handler](std::uint64_t user) { return handler.pending_messages(user); });
            server.set_message_handler([&](chatroom::ConnectionId id, std::string payload) {
                dispatcher.enqueue(id, std::move(payload));
            });
            server.set_disconnect_handler([&](chatroom::ConnectionId id) { dispatcher.disconnect(id); });
            chatroom::HttpServer http(auth, config.database, config.address, config.http, config.files,
                [&server, &dispatcher](chatroom::UserNotification notification) {
                    server.post([&dispatcher, notification = std::move(notification)] {
                        dispatcher.notify(notification);
                    });
                });
            http.start([&server] { server.stop(); });
            chatroom::log(chatroom::LogLevel::info, "server_started",
                          {{"address", config.address}, {"port", server.port()},
                           {"worker_threads", config.worker_threads},
                           {"http_port", http.port()}, {"http_workers", config.http.worker_threads},
                           {"max_connections", config.max_connections},
                           {"idle_timeout_seconds", config.idle_timeout_seconds},
                           {"max_requests_per_connection", config.max_requests_per_connection},
                           {"max_pending_requests", config.max_pending_requests}});
            phase = "runtime";
            server.run(shutdown_signal.get());
            http.stop();
            http.check();
        }
        chatroom::log(chatroom::LogLevel::info, "server_stopped");
    } catch (const std::exception& error) {
        chatroom::log(chatroom::LogLevel::error, "server_failed",
                      {{"phase", phase}, {"error", error.what()}});
        return 1;
    }
}
