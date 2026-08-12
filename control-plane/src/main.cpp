// Control plane entry point.
//
// Wires up the API contract (see docs/api.md) against a SQLite-backed
// function registry and invocation history, and a runtime manager backed
// by either Docker or Kubernetes (FAAS_RUNTIME_BACKEND, default docker).
// Runtimes themselves are not persisted (see docs/architecture.md for
// why); they are rebuilt on demand after a restart, same as after any
// scale-to-zero.

#include "faas/database.hpp"
#include "faas/docker_client.hpp"
#include "faas/function_registry.hpp"
#include "faas/invocation_store.hpp"
#include "faas/kubernetes_client.hpp"
#include "faas/metrics.hpp"
#include "faas/runtime_manager.hpp"
#include "faas_http/http_server.hpp"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <iostream>
#include <memory>

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
        {"idle_timeout_ms", fn.spec.idle_timeout_ms},
        {"status", to_string(fn.status)},
    };
}

Response error_response(int status, const std::string& message) {
    return Response::json(status, json{{"error", message}}.dump());
}

// The idle timeout a newly deployed function gets when its deploy
// request does not specify one explicitly.
long default_idle_timeout_ms() {
    const char* env = std::getenv("FAAS_IDLE_TIMEOUT_MS");
    if (!env) return 60000;
    try {
        return std::stol(env);
    } catch (const std::exception&) {
        return 60000;
    }
}

std::unique_ptr<ContainerBackend> make_runtime_backend() {
    const char* env = std::getenv("FAAS_RUNTIME_BACKEND");
    std::string kind = env ? env : "docker";
    if (kind == "kubernetes" || kind == "k8s") {
        std::cout << "runtime backend: kubernetes (kubectl)\n";
        return std::make_unique<KubernetesClient>();
    }
    std::cout << "runtime backend: docker\n";
    return std::make_unique<DockerClient>();
}

} // namespace

int main() {
    const char* db_path_env = std::getenv("FAAS_DB_PATH");
    Database db(db_path_env ? db_path_env : "faas.db");

    FunctionRegistry registry(db);
    InvocationStore invocations(db);
    Metrics metrics;
    RuntimeManager runtime_manager(metrics, make_runtime_backend());
    Server server;
    long platform_default_idle_timeout_ms = default_idle_timeout_ms();

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
        spec.idle_timeout_ms = body.value("idle_timeout_ms", platform_default_idle_timeout_ms);

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
        runtime_manager.terminate(params[0]);
        return Response::empty(204);
    });

    server.route("POST", "/functions/{name}/invoke", [&](const Request& req, const std::vector<std::string>& params) {
        auto fn = registry.get(params[0]);
        if (!fn) return error_response(404, "function not found: " + params[0]);

        std::string input = req.body.empty() ? "{}" : req.body;
        InvocationResult result = runtime_manager.invoke(fn->spec, input);

        std::string invocation_id = invocations.record(params[0], input, result.status, result.output,
                                                         result.duration_ms, result.cold_start);

        if (result.status == "infra_error") {
            json body = {
                {"invocation_id", invocation_id},
                {"status", "error"},
                {"error", result.output},
                {"duration_ms", result.duration_ms},
                {"cold_start", result.cold_start},
            };
            return Response::json(502, body.dump());
        }

        if (result.status == "timeout") {
            json body = {
                {"invocation_id", invocation_id},
                {"status", "timeout"},
                {"error", result.output},
                {"duration_ms", result.duration_ms},
                {"cold_start", result.cold_start},
            };
            return Response::json(200, body.dump());
        }

        json function_result;
        try {
            function_result = json::parse(result.output);
        } catch (const std::exception&) {
            function_result = result.output; // function did not return JSON; report it as a raw string
        }

        json body = {
            {"invocation_id", invocation_id},
            {"status", result.status},
            {"result", function_result},
            {"duration_ms", result.duration_ms},
            {"cold_start", result.cold_start},
        };
        return Response::json(200, body.dump());
    });

    server.route("GET", "/functions/{name}/runtime", [&](const Request&, const std::vector<std::string>& params) {
        auto fn = registry.get(params[0]);
        if (!fn) return error_response(404, "function not found: " + params[0]);

        auto rt = runtime_manager.status(params[0]);
        if (!rt) return error_response(404, "no runtime currently running for " + params[0]);

        json body = {
            {"state", to_string(rt->state)},
            {"container_id", rt->container_id},
            {"host_port", rt->host_port},
            {"idle_ms", rt->idle_ms},
        };
        return Response::json(200, body.dump());
    });

    server.route("GET", "/runtimes", [&](const Request&, const std::vector<std::string>&) {
        json entries = json::array();
        for (const auto& rt : runtime_manager.list_runtimes()) {
            entries.push_back({
                {"function", rt.function_name},
                {"state", to_string(rt.state)},
                {"container_id", rt.container_id},
                {"host_port", rt.host_port},
                {"idle_ms", rt.idle_ms},
            });
        }
        return Response::json(200, entries.dump());
    });

    server.route("GET", "/metrics", [&](const Request&, const std::vector<std::string>&) {
        long active_runtimes = static_cast<long>(runtime_manager.list_runtimes().size());
        Response resp = Response::text(200, metrics.render(active_runtimes));
        resp.headers["Content-Type"] = "text/plain; version=0.0.4";
        return resp;
    });

    server.route("GET", "/invocations/{id}", [&](const Request&, const std::vector<std::string>& params) {
        auto record = invocations.get(params[0]);
        if (!record) return error_response(404, "invocation not found: " + params[0]);

        json output;
        try {
            output = json::parse(record->output);
        } catch (const std::exception&) {
            output = record->output;
        }

        json body = {
            {"id", record->id},
            {"function", record->function_name},
            {"status", record->status},
            {"result", output},
            {"duration_ms", record->duration_ms},
            {"cold_start", record->cold_start},
        };
        return Response::json(200, body.dump());
    });

    const char* host_env = std::getenv("FAAS_HOST");
    const char* port_env = std::getenv("FAAS_PORT");
    std::string host = host_env ? host_env : "0.0.0.0";
    int port = port_env ? std::atoi(port_env) : 8080;

    std::cout << "control plane listening on " << host << ":" << port << "\n";
    server.listen(host, port);

    return 0;
}
