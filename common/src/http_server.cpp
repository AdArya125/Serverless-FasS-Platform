#include "faas_http/http_server.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace faas_http {

namespace {

std::vector<std::string> split_path(const std::string& path) {
    std::vector<std::string> segments;
    std::string current;
    for (char c : path) {
        if (c == '?') {
            break; // query string is not part of routing
        } else if (c == '/') {
            if (!current.empty()) {
                segments.push_back(current);
                current.clear();
            }
        } else {
            current += c;
        }
    }
    if (!current.empty()) segments.push_back(current);
    return segments;
}

std::string reason_phrase(int status) {
    switch (status) {
        case 200: return "OK";
        case 201: return "Created";
        case 204: return "No Content";
        case 400: return "Bad Request";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 409: return "Conflict";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        default: return "";
    }
}

// Reads from fd until the "\r\n\r\n" header terminator is seen. Returns
// the header block and any body bytes that were read past it.
bool read_headers(int fd, std::string& header_block, std::string& leftover) {
    char buf[4096];
    std::string data;
    size_t pos;
    while ((pos = data.find("\r\n\r\n")) == std::string::npos) {
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) return false;
        data.append(buf, static_cast<size_t>(n));
        if (data.size() > (1u << 20)) return false; // header block too large
    }
    header_block = data.substr(0, pos);
    leftover = data.substr(pos + 4);
    return true;
}

} // namespace

Response Response::json(int status, const std::string& body) {
    Response r;
    r.status = status;
    r.headers["Content-Type"] = "application/json";
    r.body = body;
    return r;
}

Response Response::text(int status, const std::string& body) {
    Response r;
    r.status = status;
    r.headers["Content-Type"] = "text/plain";
    r.body = body;
    return r;
}

Response Response::empty(int status) {
    Response r;
    r.status = status;
    return r;
}

void Server::route(const std::string& method, const std::string& pattern, Handler handler) {
    Route route;
    route.method = method;
    route.segments = split_path(pattern);
    route.handler = std::move(handler);
    routes_.push_back(std::move(route));
}

bool Server::match(const Route& route, const std::vector<std::string>& segments,
                    std::vector<std::string>& params) const {
    if (route.segments.size() != segments.size()) return false;
    params.clear();
    for (size_t i = 0; i < segments.size(); ++i) {
        const std::string& expected = route.segments[i];
        if (!expected.empty() && expected.front() == '{' && expected.back() == '}') {
            params.push_back(segments[i]);
        } else if (expected != segments[i]) {
            return false;
        }
    }
    return true;
}

void Server::handle_connection(int client_fd) {
    std::string header_block, leftover;
    if (!read_headers(client_fd, header_block, leftover)) {
        close(client_fd);
        return;
    }

    std::istringstream stream(header_block);
    std::string request_line;
    std::getline(stream, request_line);
    if (!request_line.empty() && request_line.back() == '\r') request_line.pop_back();

    Request req;
    std::istringstream rl(request_line);
    std::string http_version;
    rl >> req.method >> req.path >> http_version;

    std::string header_line;
    while (std::getline(stream, header_line)) {
        if (!header_line.empty() && header_line.back() == '\r') header_line.pop_back();
        if (header_line.empty()) continue;
        auto colon = header_line.find(':');
        if (colon == std::string::npos) continue;
        std::string key = header_line.substr(0, colon);
        std::string value = header_line.substr(colon + 1);
        while (!value.empty() && value.front() == ' ') value.erase(value.begin());
        req.headers[key] = value;
    }

    size_t content_length = 0;
    auto it = req.headers.find("Content-Length");
    if (it != req.headers.end()) content_length = std::stoul(it->second);

    std::string body = leftover;
    while (body.size() < content_length) {
        char buf[4096];
        ssize_t n = recv(client_fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        body.append(buf, static_cast<size_t>(n));
    }
    req.body = body.substr(0, std::min(body.size(), content_length));

    auto segments = split_path(req.path);

    Response resp;
    bool handled = false;
    bool path_matched_any_method = false;
    for (const auto& route : routes_) {
        std::vector<std::string> params;
        if (!match(route, segments, params)) continue;
        path_matched_any_method = true;
        if (route.method != req.method) continue;
        resp = route.handler(req, params);
        handled = true;
        break;
    }
    if (!handled) {
        resp = path_matched_any_method
                   ? Response::json(405, R"({"error":"method not allowed"})")
                   : Response::json(404, R"({"error":"not found"})");
    }

    resp.headers["Content-Length"] = std::to_string(resp.body.size());
    resp.headers["Connection"] = "close";

    std::ostringstream out;
    out << "HTTP/1.1 " << resp.status << " " << reason_phrase(resp.status) << "\r\n";
    for (const auto& header : resp.headers) {
        out << header.first << ": " << header.second << "\r\n";
    }
    out << "\r\n" << resp.body;

    std::string out_str = out.str();
    send(client_fd, out_str.data(), out_str.size(), 0);
    close(client_fd);
}

void Server::listen(const std::string& host, int port) {
    listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) throw std::runtime_error("failed to create listening socket");

    int opt = 1;
    setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    addr.sin_addr.s_addr = (host == "0.0.0.0") ? INADDR_ANY : inet_addr(host.c_str());

    if (bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        throw std::runtime_error("failed to bind to " + host + ":" + std::to_string(port));
    }
    if (::listen(listen_fd_, 64) < 0) {
        throw std::runtime_error("failed to listen on " + host + ":" + std::to_string(port));
    }

    running_ = true;
    while (running_) {
        sockaddr_in client_addr{};
        socklen_t len = sizeof(client_addr);
        int client_fd = accept(listen_fd_, reinterpret_cast<sockaddr*>(&client_addr), &len);
        if (client_fd < 0) {
            if (!running_) break;
            continue;
        }
        std::thread(&Server::handle_connection, this, client_fd).detach();
    }
}

void Server::stop() {
    running_ = false;
    if (listen_fd_ >= 0) {
        shutdown(listen_fd_, SHUT_RDWR);
        close(listen_fd_);
        listen_fd_ = -1;
    }
}

} // namespace faas_http
