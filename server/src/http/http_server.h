#pragma once

#include "auth/auth_service.h"
#include "notification.h"

#include <atomic>
#include <exception>
#include <functional>
#include <httplib.h>
#include <thread>

namespace chatroom {

// HTTP/1.1 authentication and file listener, independent of the TCP Reactor.
// Binds during construction; start once. Stop before destroying the borrowed AuthService.
class HttpServer {
public:
    using NotificationHandler = std::function<void(UserNotification)>;
    HttpServer(const AuthService& auth, DatabaseConfig database, const std::string& address,
               HttpConfig config = {}, FileConfig files = {}, NotificationHandler notify = {});
    ~HttpServer();
    std::uint16_t port() const { return port_; }
    void start(std::function<void()> on_failure = {});
    void stop();
    // Rethrows a listener failure after stop() has joined its thread.
    void check() const;

private:
    void handle(const httplib::Request& request, httplib::Response& response,
                const httplib::ContentReader& reader, bool registration);
    // Both operations authenticate on the HTTP worker; uploads persist before notification.
    void handle_file(const httplib::Request& request, httplib::Response& response,
                     const httplib::ContentReader* reader = nullptr);

    const AuthService& auth_;
    DatabaseConfig database_;
    FileConfig files_;
    NotificationHandler notify_;
    httplib::Server server_;
    std::uint16_t port_;
    std::atomic<bool> stopping_{false};
    std::thread listener_;
    std::exception_ptr failure_;
};

} // namespace chatroom
