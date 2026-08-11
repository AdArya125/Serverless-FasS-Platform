#pragma once
// Minimal HTTP/1.1 client built directly on POSIX sockets, matching the
// request/response style of http_server.hpp. Used by the CLI to talk to
// the control plane, and later by the runtime manager to invoke function
// containers.

#include <map>
#include <string>

namespace faas_http {

struct ClientResponse {
    int status = 0;
    std::map<std::string, std::string> headers;
    std::string body;
    bool ok = false;    // true if a response was successfully read
    std::string error;  // populated when ok is false
};

class Client {
public:
    Client(std::string host, int port);

    ClientResponse get(const std::string& path);
    ClientResponse post(const std::string& path, const std::string& body,
                         const std::string& content_type = "application/json");
    ClientResponse del(const std::string& path);

private:
    ClientResponse request(const std::string& method, const std::string& path,
                            const std::string& body, const std::string& content_type);

    std::string host_;
    int port_;
};

} // namespace faas_http
