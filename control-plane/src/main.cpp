// Control plane entry point.
//
// Wires up the API contract (see docs/api.md) against a purely
// in-memory function registry. Invocation and invocation-history
// endpoints are stubbed out until the runtime manager and persistence
// layer exist.

#include "faas/function_registry.hpp"
#include "faas_http/http_server.hpp"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <iostream>

using json = nlohmann::json;
using namespace faas;
using namespace faas_http;

namespace {

json function_to_json(const Function& fn) {
    return json{
        {"name", fn.spec.name},
        {"version", fn.spec.version},
        {"image", fn.spec.image},
        {"timeout_ms", fn.spec.timeout_ms},
        {"memory_mb", fn.spec.memory_mb},
        {"cpu", fn.spec.cpu},
        {"max_concurrency", fn.spec.max_concurrency},
        {"status", to_string(fn.status)},
    };
}

Response error_response(int status, const std::string& message) {
    return Response::json(status, json{{"error", message}}.dump());
}

} // namespace

int main() {
    FunctionRegistry registry;
    Server server;

    server.route("GET", "/health", [](const Request&, const std::vector<std::string>&) {
        return Response::json(200, json{{"status", "ok"}}.dump());
    });

    server.route("POST", "/functions", [&](const Request& req, const std::vector<std::string>&) {
        json body;
        try {
            body = json::parse(req.body);
        } catch (const std::exception&) {
            return error_response(400, "invalid JSON body");
        }
        if (!body.contains("name") || !body.contains("image")) {
            return error_response(400, "'name' and 'image' are required");
        }

        FunctionSpec spec;
        spec.name = body.at("name").get<std::string>();
        spec.image = body.at("image").get<std::string>();
        spec.version = body.value("version", 1);
        spec.timeout_ms = body.value("timeout_ms", 5000);
        spec.memory_mb = body.value("memory_mb", 256);
        spec.cpu = body.value("cpu", 0.5);
        spec.max_concurrency = body.value("max_concurrency", 1);

        Function fn = registry.register_function(spec);
        return Response::json(201, function_to_json(fn).dump());
    });

    server.route("GET", "/functions/{name}", [&](const Request&, const std::vector<std::string>& params) {
        auto fn = registry.get(params[0]);
        if (!fn) return error_response(404, "function not found: " + params[0]);
        return Response::json(200, function_to_json(*fn).dump());
    });

    server.route("DELETE", "/functions/{name}", [&](const Request&, const std::vector<std::string>& params) {
        if (!registry.remove(params[0])) return error_response(404, "function not found: " + params[0]);
        return Response::empty(204);
    });

    server.route("POST", "/functions/{name}/invoke", [&](const Request&, const std::vector<std::string>& params) {
        auto fn = registry.get(params[0]);
        if (!fn) return error_response(404, "function not found: " + params[0]);
        return error_response(501, "invocation is not implemented yet");
    });

    server.route("GET", "/invocations/{id}", [&](const Request&, const std::vector<std::string>&) {
        return error_response(501, "invocation records are not implemented yet");
    });

    const char* host_env = std::getenv("FAAS_HOST");
    const char* port_env = std::getenv("FAAS_PORT");
    std::string host = host_env ? host_env : "0.0.0.0";
    int port = port_env ? std::atoi(port_env) : 8080;

    std::cout << "control plane listening on " << host << ":" << port << "\n";
    server.listen(host, port);

    return 0;
}
