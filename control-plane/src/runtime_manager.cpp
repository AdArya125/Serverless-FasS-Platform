#include "faas/runtime_manager.hpp"

#include "faas_http/http_client.hpp"

namespace faas {

namespace {
constexpr int kContainerPort = 8080;
constexpr int kReadinessAttempts = 50;
constexpr int kReadinessIntervalMs = 100; // ~5s worst case startup wait
constexpr int kHealthCheckTimeoutMs = 2000;
constexpr int kReaperIntervalMs = 500;

long elapsed_ms(std::chrono::steady_clock::time_point start) {
    auto elapsed = std::chrono::steady_clock::now() - start;
    return std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
}
} // namespace

RuntimeManager::RuntimeManager(Metrics& metrics, std::unique_ptr<ContainerBackend> backend)
    : backend_(std::move(backend)), metrics_(metrics), reaper_thread_(&RuntimeManager::reap_idle_runtimes, this) {}

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
    ContainerBackend::RunResult run = backend_->run_container(spec.image, kContainerPort);
    if (!run.ok) {
        error = "failed to start container: " + run.error;
        return false;
    }

    int port = backend_->get_host_port(run.id, kContainerPort);
    if (port <= 0) {
        backend_->remove_container(run.id);
        error = "failed to determine host port for container " + run.id;
        return false;
    }

    for (int attempt = 0; attempt < kReadinessAttempts; ++attempt) {
        if (is_healthy(port)) {
            out.container_id = run.id;
            out.host_port = port;
            out.idle_timeout_ms = spec.idle_timeout_ms;
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(kReadinessIntervalMs));
    }

    backend_->remove_container(run.id);
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
            bool expired = it->second.state == RuntimeState::IDLE && idle_ms >= it->second.idle_timeout_ms;
            if (expired) {
                backend_->remove_container(it->second.container_id);
                it = runtimes_.erase(it);
                metrics_.record_runtime_terminated("idle_timeout");
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

std::vector<RuntimeListEntry> RuntimeManager::list_runtimes() {
    std::lock_guard<std::mutex> lock(mutex_);
    auto now = std::chrono::steady_clock::now();

    std::vector<RuntimeListEntry> entries;
    entries.reserve(runtimes_.size());
    for (const auto& pair : runtimes_) {
        RuntimeListEntry entry;
        entry.function_name = pair.first;
        entry.state = pair.second.state;
        entry.container_id = pair.second.container_id;
        entry.host_port = pair.second.host_port;
        entry.idle_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(now - pair.second.last_used_at).count();
        entries.push_back(entry);
    }
    return entries;
}

void RuntimeManager::terminate(const std::string& function_name) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = runtimes_.find(function_name);
    if (it == runtimes_.end()) return;

    backend_->remove_container(it->second.container_id);
    runtimes_.erase(it);
    metrics_.record_runtime_terminated("deleted");
}

InvocationResult RuntimeManager::try_invoke(const FunctionSpec& spec, const std::string& input_json,
                                             std::chrono::steady_clock::time_point start,
                                             bool& connection_failed) {
    connection_failed = false;
    InvocationResult result;

    std::unique_lock<std::mutex> lock(mutex_);

    auto it = runtimes_.find(spec.name);
    bool need_start = (it == runtimes_.end());

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
        metrics_.record_runtime_created();
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
            backend_->remove_container(container_id);
            runtimes_.erase(it);
            metrics_.record_runtime_terminated("timeout");
        }
    } else if (!resp.ok) {
        connection_failed = true;
        result.status = "infra_error";
        result.output = resp.error;
        if (still_tracked) {
            backend_->remove_container(container_id);
            runtimes_.erase(it);
            metrics_.record_runtime_terminated("dead");
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

InvocationResult RuntimeManager::invoke(const FunctionSpec& spec, const std::string& input_json) {
    auto start = std::chrono::steady_clock::now();

    bool connection_failed = false;
    InvocationResult result = try_invoke(spec, input_json, start, connection_failed);

    // A warm runtime is trusted without a pre-flight health check, so if
    // it turns out to be dead (the container crashed or was removed
    // since the last call), the caller sees a spurious failure unless we
    // recover here: retry exactly once against a freshly created
    // runtime. A cold attempt that fails this way is not retried again -
    // that would just loop on a genuinely broken image.
    if (connection_failed && !result.cold_start) {
        result = try_invoke(spec, input_json, start, connection_failed);
    }

    metrics_.record_invocation(result.status, result.cold_start, static_cast<double>(result.duration_ms));
    return result;
}

} // namespace faas
