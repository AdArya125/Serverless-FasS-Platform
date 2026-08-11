#pragma once
// Minimal HTTP/1.1 server built directly on POSIX sockets.
//
// Each connection is handled on its own thread and closed after a single
// request/response (Connection: close). That is enough for the control
// plane's request/response API and keeps the implementation small enough
// to read in one sitting.

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace faas_http {

struct Request {
    std::string method;
    std::string path;
    std::map<std::string, std::string> headers;
    std::string body;
};

struct Response {
    int status = 200;
    std::map<std::string, std::string> headers;
    std::string body;

    static Response json(int status, const std::string& body);
    static Response text(int status, const std::string& body);
    static Response empty(int status);
};

class Server {
public:
    // params holds the values captured by "{name}" segments in the route
    // pattern, in the order they appear.
    using Handler = std::function<Response(const Request&, const std::vector<std::string>& params)>;

    void route(const std::string& method, const std::string& pattern, Handler handler);

    // Blocks the calling thread, serving requests until stop() is called
    // from another thread.
    void listen(const std::string& host, int port);
    void stop();

private:
    struct Route {
        std::string method;
        std::vector<std::string> segments;
        Handler handler;
    };

    bool match(const Route& route, const std::vector<std::string>& segments,
               std::vector<std::string>& params) const;
    void handle_connection(int client_fd);

    std::vector<Route> routes_;
    int listen_fd_ = -1;
    volatile bool running_ = false;
};

} // namespace faas_http
