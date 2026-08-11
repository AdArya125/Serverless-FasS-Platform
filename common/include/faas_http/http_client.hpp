#pragma once
// Minimal HTTP/1.1 client built directly on POSIX sockets, matching the
// request/response style of http_server.hpp. Used by the CLI to talk to
// the control plane, and by the runtime manager to invoke function
// containers.

#include <map>
#include <string>

namespace faas_http {

struct ClientResponse {
    int status = 0;
    std::map<std::string, std::string> headers;
    std::string body;
    bool ok = false;        // true if a complete response was read
    bool timed_out = false; // true if timeout_ms elapsed waiting on the socket
    std::string error;      // populated when ok is false
};

class Client {
public:
    Client(std::string host, int port);

    // timeout_ms applies to each individual socket read/write, not to
    // the call as a whole; 0 means block indefinitely (the previous
    // behavior). This is enough to detect a peer that never responds at
    // all, which is the failure mode the runtime manager needs to catch.
    ClientResponse get(const std::string& path, int timeout_ms = 0);
    ClientResponse post(const std::string& path, const std::string& body,
                         const std::string& content_type = "application/json", int timeout_ms = 0);
    ClientResponse del(const std::string& path, int timeout_ms = 0);

private:
    ClientResponse request(const std::string& method, const std::string& path,
                            const std::string& body, const std::string& content_type,
                            int timeout_ms);

    std::string host_;
    int port_;
};

} // namespace faas_http
