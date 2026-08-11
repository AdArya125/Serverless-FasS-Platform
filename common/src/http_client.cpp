#include "faas_http/http_client.hpp"

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <sstream>

namespace faas_http {

Client::Client(std::string host, int port) : host_(std::move(host)), port_(port) {}

ClientResponse Client::get(const std::string& path, int timeout_ms) {
    return request("GET", path, "", "", timeout_ms);
}

ClientResponse Client::post(const std::string& path, const std::string& body, const std::string& content_type,
                             int timeout_ms) {
    return request("POST", path, body, content_type, timeout_ms);
}

ClientResponse Client::del(const std::string& path, int timeout_ms) {
    return request("DELETE", path, "", "", timeout_ms);
}

ClientResponse Client::request(const std::string& method, const std::string& path,
                                const std::string& body, const std::string& content_type,
                                int timeout_ms) {
    ClientResponse result;

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        result.error = "failed to create socket";
        return result;
    }

    if (timeout_ms > 0) {
        struct timeval tv;
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port_));

    if (inet_pton(AF_INET, host_.c_str(), &addr.sin_addr) != 1) {
        hostent* resolved = gethostbyname(host_.c_str());
        if (!resolved || resolved->h_length <= 0) {
            result.error = "could not resolve host: " + host_;
            close(sock);
            return result;
        }
        std::memcpy(&addr.sin_addr, resolved->h_addr_list[0], static_cast<size_t>(resolved->h_length));
    }

    if (connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        result.error = "connection refused at " + host_ + ":" + std::to_string(port_);
        close(sock);
        return result;
    }

    std::ostringstream req;
    req << method << " " << path << " HTTP/1.1\r\n";
    req << "Host: " << host_ << "\r\n";
    req << "Connection: close\r\n";
    if (!body.empty()) {
        req << "Content-Type: " << content_type << "\r\n";
        req << "Content-Length: " << body.size() << "\r\n";
    }
    req << "\r\n" << body;

    std::string req_str = req.str();
    if (send(sock, req_str.data(), req_str.size(), 0) < 0) {
        result.error = "failed to send request";
        close(sock);
        return result;
    }

    std::string data;
    char buf[4096];
    ssize_t n;
    while ((n = recv(sock, buf, sizeof(buf), 0)) > 0) {
        data.append(buf, static_cast<size_t>(n));
    }
    bool recv_timed_out = (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK));
    close(sock);

    if (recv_timed_out) {
        result.timed_out = true;
        result.error = "request timed out after " + std::to_string(timeout_ms) + "ms";
        return result;
    }

    if (data.empty()) {
        result.error = "empty response from server";
        return result;
    }

    size_t header_end = data.find("\r\n\r\n");
    if (header_end == std::string::npos) {
        result.error = "malformed response";
        return result;
    }

    std::istringstream stream(data.substr(0, header_end));
    std::string status_line;
    std::getline(stream, status_line);

    std::istringstream sl(status_line);
    std::string version;
    sl >> version >> result.status;

    std::string header_line;
    while (std::getline(stream, header_line)) {
        if (!header_line.empty() && header_line.back() == '\r') header_line.pop_back();
        auto colon = header_line.find(':');
        if (colon == std::string::npos) continue;
        std::string key = header_line.substr(0, colon);
        std::string value = header_line.substr(colon + 1);
        while (!value.empty() && value.front() == ' ') value.erase(value.begin());
        result.headers[key] = value;
    }

    result.body = data.substr(header_end + 4);
    result.ok = true;
    return result;
}

} // namespace faas_http
