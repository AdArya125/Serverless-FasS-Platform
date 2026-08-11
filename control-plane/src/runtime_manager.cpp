#include "faas/runtime_manager.hpp"

#include "faas_http/http_client.hpp"

#include <chrono>
#include <thread>

namespace faas {

namespace {
constexpr int kContainerPort = 8080;
constexpr int kReadinessAttempts = 50;
constexpr int kReadinessIntervalMs = 100; // ~5s worst case startup wait
} // namespace

bool RuntimeManager::is_healthy(int host_port) {
    faas_http::Client client("127.0.0.1", host_port);
    auto resp = client.get("/health");
    return resp.ok && resp.status >= 200 && resp.status < 300;
}

bool RuntimeManager::start_runtime(const FunctionSpec& spec, Runtime& out, std::string& error) {
    DockerClient::RunResult run = docker_.run_container(spec.image, kContainerPort);
    if (!run.ok) {
        error = "failed to start container: " + run.error;
        return false;
    }

    int port = docker_.get_host_port(run.container_id, kContainerPort);
    if (port <= 0) {
        docker_.remove_container(run.container_id);
        error = "failed to determine host port for container " + run.container_id;
        return false;
    }

    for (int attempt = 0; attempt < kReadinessAttempts; ++attempt) {
        if (is_healthy(port)) {
            out.container_id = run.container_id;
            out.host_port = port;
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(kReadinessIntervalMs));
    }

    docker_.remove_container(run.container_id);
    error = "container did not become healthy within the startup window";
    return false;
}

InvocationResult RuntimeManager::invoke(const FunctionSpec& spec, const std::string& input_json) {
    auto start = std::chrono::steady_clock::now();
    InvocationResult result;

    std::unique_lock<std::mutex> lock(mutex_);

    auto it = runtimes_.find(spec.name);
    bool need_start = (it == runtimes_.end());
    if (!need_start && !is_healthy(it->second.host_port)) {
        // The previously created container is no longer answering; drop
        // it rather than silently reusing a broken runtime.
        docker_.remove_container(it->second.container_id);
        runtimes_.erase(it);
        need_start = true;
    }

    Runtime runtime;
    if (need_start) {
        std::string error;
        if (!start_runtime(spec, runtime, error)) {
            lock.unlock();
            auto elapsed = std::chrono::steady_clock::now() - start;
            result.status = "infra_error";
            result.output = error;
            result.duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
            return result;
        }
        runtimes_[spec.name] = runtime;
        result.cold_start = true;
    } else {
        runtime = runtimes_.at(spec.name);
        result.cold_start = false;
    }
    lock.unlock();

    faas_http::Client client("127.0.0.1", runtime.host_port);
    auto resp = client.post("/invoke", input_json);

    auto elapsed = std::chrono::steady_clock::now() - start;
    result.duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();

    if (!resp.ok) {
        result.status = "infra_error";
        result.output = resp.error;
    } else if (resp.status >= 200 && resp.status < 300) {
        result.status = "success";
        result.output = resp.body;
    } else {
        result.status = "error";
        result.output = resp.body;
    }
    return result;
}

} // namespace faas
