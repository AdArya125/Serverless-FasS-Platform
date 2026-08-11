#include "faas/runtime_manager.hpp"

#include "faas_http/http_client.hpp"

#include <cstdlib>

namespace faas {

namespace {
constexpr int kContainerPort = 8080;
constexpr int kReadinessAttempts = 50;
constexpr int kReadinessIntervalMs = 100; // ~5s worst case startup wait
constexpr int kHealthCheckTimeoutMs = 2000;
constexpr long kDefaultIdleTimeoutMs = 60000;
constexpr int kReaperIntervalMs = 500;

long read_idle_timeout_ms() {
    const char* env = std::getenv("FAAS_IDLE_TIMEOUT_MS");
    if (!env) return kDefaultIdleTimeoutMs;
    try {
        return std::stol(env);
    } catch (const std::exception&) {
        return kDefaultIdleTimeoutMs;
    }
}

long elapsed_ms(std::chrono::steady_clock::time_point start) {
    auto elapsed = std::chrono::steady_clock::now() - start;
    return std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
}
} // namespace

RuntimeManager::RuntimeManager()
    : idle_timeout_ms_(read_idle_timeout_ms()), reaper_thread_(&RuntimeManager::reap_idle_runtimes, this) {}

RuntimeManager::~RuntimeManager() {
    running_ = false;
    if (reaper_thread_.joinable()) reaper_thread_.join();
}

bool RuntimeManager::is_healthy(int host_port) {
    faas_http::Client client("127.0.0.1", host_port);
    auto resp = client.get("/health", kHealthCheckTimeoutMs);
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

void RuntimeManager::reap_idle_runtimes() {
    while (running_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(kReaperIntervalMs));

        std::lock_guard<std::mutex> lock(mutex_);
        auto now = std::chrono::steady_clock::now();
        for (auto it = runtimes_.begin(); it != runtimes_.end();) {
            long idle_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - it->second.last_used_at).count();
            bool expired = it->second.state == RuntimeState::IDLE && idle_ms >= idle_timeout_ms_;
            if (expired) {
                docker_.remove_container(it->second.container_id);
                it = runtimes_.erase(it);
            } else {
                ++it;
            }
        }
    }
}

std::optional<RuntimeStatus> RuntimeManager::status(const std::string& function_name) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = runtimes_.find(function_name);
    if (it == runtimes_.end()) return std::nullopt;

    RuntimeStatus s;
    s.state = it->second.state;
    s.container_id = it->second.container_id;
    s.host_port = it->second.host_port;
    s.idle_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - it->second.last_used_at)
                    .count();
    return s;
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

    if (need_start) {
        Runtime runtime;
        std::string error;
        if (!start_runtime(spec, runtime, error)) {
            lock.unlock();
            result.status = "infra_error";
            result.output = error;
            result.duration_ms = elapsed_ms(start);
            return result;
        }
        result.cold_start = true;
        it = runtimes_.emplace(spec.name, runtime).first;
    } else {
        result.cold_start = false;
    }

    it->second.state = RuntimeState::BUSY;
    it->second.last_used_at = std::chrono::steady_clock::now();
    int host_port = it->second.host_port;
    std::string container_id = it->second.container_id;

    lock.unlock();

    faas_http::Client client("127.0.0.1", host_port);
    auto resp = client.post("/invoke", input_json, "application/json", spec.timeout_ms);

    result.duration_ms = elapsed_ms(start);

    lock.lock();
    it = runtimes_.find(spec.name);
    bool still_tracked = (it != runtimes_.end() && it->second.container_id == container_id);

    if (resp.timed_out) {
        result.status = "timeout";
        result.output = "function exceeded timeout of " + std::to_string(spec.timeout_ms) + "ms";
        if (still_tracked) {
            docker_.remove_container(container_id);
            runtimes_.erase(it);
        }
    } else if (!resp.ok) {
        result.status = "infra_error";
        result.output = resp.error;
        if (still_tracked) {
            docker_.remove_container(container_id);
            runtimes_.erase(it);
        }
    } else if (resp.status >= 200 && resp.status < 300) {
        result.status = "success";
        result.output = resp.body;
        if (still_tracked) {
            it->second.state = RuntimeState::IDLE;
            it->second.last_used_at = std::chrono::steady_clock::now();
        }
    } else {
        result.status = "error";
        result.output = resp.body;
        if (still_tracked) {
            it->second.state = RuntimeState::IDLE;
            it->second.last_used_at = std::chrono::steady_clock::now();
        }
    }

    return result;
}

} // namespace faas
