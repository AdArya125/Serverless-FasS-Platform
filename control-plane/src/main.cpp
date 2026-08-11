// Control plane entry point.
//
// Wires up the API contract (see docs/api.md) against a purely
// in-memory function registry and a Docker-backed runtime manager.
// Invocation history is stubbed out until the persistence layer exists.

#include "faas/function_registry.hpp"
#include "faas/runtime_manager.hpp"
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
    RuntimeManager runtime_manager;
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

    server.route("POST", "/functions/{name}/invoke", [&](const Request& req, const std::vector<std::string>& params) {
        auto fn = registry.get(params[0]);
        if (!fn) return error_response(404, "function not found: " + params[0]);

        std::string input = req.body.empty() ? "{}" : req.body;
        InvocationResult result = runtime_manager.invoke(fn->spec, input);

        if (result.status == "infra_error") {
            json body = {
                {"status", "error"},
                {"error", result.output},
                {"duration_ms", result.duration_ms},
                {"cold_start", result.cold_start},
            };
            return Response::json(502, body.dump());
        }

        json function_result;
        try {
            function_result = json::parse(result.output);
        } catch (const std::exception&) {
            function_result = result.output; // function did not return JSON; report it as a raw string
        }

        json body = {
            {"status", result.status},
            {"result", function_result},
            {"duration_ms", result.duration_ms},
            {"cold_start", result.cold_start},
        };
        return Response::json(200, body.dump());
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
