// cloudfn: command-line client for the FaaS control plane.
//
// Talks to the control plane over the HTTP API defined in docs/api.md.
// The control plane address is read from FAAS_API (default
// 127.0.0.1:8080).

#include "faas_http/http_client.hpp"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <iostream>
#include <map>
#include <string>
#include <vector>

using json = nlohmann::json;
using faas_http::Client;

namespace {

struct Args {
    std::vector<std::string> positional;
    std::map<std::string, std::string> flags;
};

Args parse_args(int argc, char** argv, int start) {
    Args args;
    for (int i = start; i < argc; ++i) {
        std::string token = argv[i];
        if (token.rfind("--", 0) == 0) {
            std::string key = token.substr(2);
            args.flags[key] = (i + 1 < argc) ? argv[++i] : "";
        } else {
            args.positional.push_back(token);
        }
    }
    return args;
}

// Parses durations like "5s" or "500ms" into milliseconds.
int parse_duration_ms(const std::string& value, int fallback) {
    if (value.empty()) return fallback;
    if (value.size() > 2 && value.substr(value.size() - 2) == "ms") {
        return std::stoi(value.substr(0, value.size() - 2));
    }
    if (value.back() == 's') {
        return std::stoi(value.substr(0, value.size() - 1)) * 1000;
    }
    return std::stoi(value);
}

// Parses memory sizes like "256Mi" or "1Gi" into megabytes.
int parse_memory_mb(const std::string& value, int fallback) {
    if (value.empty()) return fallback;
    if (value.size() > 2 && value.substr(value.size() - 2) == "Mi") {
        return std::stoi(value.substr(0, value.size() - 2));
    }
    if (value.size() > 2 && value.substr(value.size() - 2) == "Gi") {
        return std::stoi(value.substr(0, value.size() - 2)) * 1024;
    }
    return std::stoi(value);
}

Client make_client() {
    const char* env = std::getenv("FAAS_API");
    std::string endpoint = env ? env : "127.0.0.1:8080";
    auto colon = endpoint.find(':');
    std::string host = endpoint.substr(0, colon);
    int port = std::stoi(endpoint.substr(colon + 1));
    return Client(host, port);
}

void print_api_error(const faas_http::ClientResponse& resp) {
    if (!resp.ok) {
        std::cerr << "error: could not reach control plane (" << resp.error << ")\n";
        std::cerr << "is the control plane running? see: make run\n";
        return;
    }
    try {
        json body = json::parse(resp.body);
        std::cerr << "error: " << body.value("error", resp.body) << "\n";
    } catch (const std::exception&) {
        std::cerr << "error: " << resp.body << "\n";
    }
}

int cmd_deploy(const Args& args) {
    if (args.positional.empty()) {
        std::cerr << "usage: cloudfn deploy <name> --image <image> "
                     "[--timeout 5s] [--memory 256Mi] [--cpu 0.5] [--concurrency 1]\n";
        return 1;
    }
    if (!args.flags.count("image") || args.flags.at("image").empty()) {
        std::cerr << "error: --image is required\n";
        return 1;
    }

    json body;
    body["name"] = args.positional[0];
    body["image"] = args.flags.at("image");
    if (args.flags.count("timeout")) body["timeout_ms"] = parse_duration_ms(args.flags.at("timeout"), 5000);
    if (args.flags.count("memory")) body["memory_mb"] = parse_memory_mb(args.flags.at("memory"), 256);
    if (args.flags.count("cpu")) body["cpu"] = std::stod(args.flags.at("cpu"));
    if (args.flags.count("concurrency")) body["max_concurrency"] = std::stoi(args.flags.at("concurrency"));

    auto resp = make_client().post("/functions", body.dump());
    if (!resp.ok || resp.status >= 300) {
        print_api_error(resp);
        return 1;
    }
    std::cout << "deployed " << args.positional[0] << "\n";
    return 0;
}

int cmd_describe(const Args& args) {
    if (args.positional.empty()) {
        std::cerr << "usage: cloudfn describe <name>\n";
        return 1;
    }
    auto resp = make_client().get("/functions/" + args.positional[0]);
    if (!resp.ok || resp.status >= 300) {
        print_api_error(resp);
        return 1;
    }
    json fn = json::parse(resp.body);
    std::cout << "Name:        " << fn.value("name", "") << "\n";
    std::cout << "Version:     " << fn.value("version", 1) << "\n";
    std::cout << "Image:       " << fn.value("image", "") << "\n";
    std::cout << "Timeout:     " << fn.value("timeout_ms", 0) << "ms\n";
    std::cout << "Memory:      " << fn.value("memory_mb", 0) << "Mi\n";
    std::cout << "CPU:         " << fn.value("cpu", 0.0) << "\n";
    std::cout << "Concurrency: " << fn.value("max_concurrency", 1) << "\n";
    std::cout << "Status:      " << fn.value("status", "") << "\n";
    return 0;
}

int cmd_invoke(const Args& args) {
    if (args.positional.empty()) {
        std::cerr << "usage: cloudfn invoke <name> --data '<json>'\n";
        return 1;
    }
    std::string data = args.flags.count("data") ? args.flags.at("data") : "{}";
    auto resp = make_client().post("/functions/" + args.positional[0] + "/invoke", data);
    if (!resp.ok) {
        print_api_error(resp);
        return 1;
    }

    json body;
    try {
        body = json::parse(resp.body);
    } catch (const std::exception&) {
        std::cerr << "error: unexpected response: " << resp.body << "\n";
        return 1;
    }

    if (resp.status >= 300) {
        std::cerr << "error: " << body.value("error", resp.body) << "\n";
        return 1;
    }

    std::cout << "status: " << body.value("status", "") << "\n";
    std::cout << "result: " << (body.contains("result") ? body["result"].dump() : "null") << "\n";
    std::cout << "duration: " << body.value("duration_ms", 0) << " ms\n";
    std::cout << "cold_start: " << (body.value("cold_start", false) ? "true" : "false") << "\n";

    return body.value("status", "") == "success" ? 0 : 1;
}

int cmd_logs(const Args& args) {
    if (args.positional.empty()) {
        std::cerr << "usage: cloudfn logs <name>\n";
        return 1;
    }
    std::cout << "logs are not implemented yet\n";
    return 0;
}

int cmd_delete(const Args& args) {
    if (args.positional.empty()) {
        std::cerr << "usage: cloudfn delete <name>\n";
        return 1;
    }
    auto resp = make_client().del("/functions/" + args.positional[0]);
    if (!resp.ok || resp.status >= 300) {
        print_api_error(resp);
        return 1;
    }
    std::cout << "deleted " << args.positional[0] << "\n";
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: cloudfn <deploy|describe|invoke|logs|delete> ...\n";
        return 1;
    }

    std::string command = argv[1];
    Args args = parse_args(argc, argv, 2);

    if (command == "deploy") return cmd_deploy(args);
    if (command == "describe") return cmd_describe(args);
    if (command == "invoke") return cmd_invoke(args);
    if (command == "logs") return cmd_logs(args);
    if (command == "delete") return cmd_delete(args);

    std::cerr << "unknown command: " << command << "\n";
    return 1;
}
