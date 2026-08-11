#pragma once

#include "faas/docker_client.hpp"
#include "faas/function.hpp"
#include "faas/runtime_state.hpp"

#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace faas {

struct InvocationResult {
    // "success"     - the function ran and returned a 2xx response
    // "error"       - the function ran but returned a non-2xx response
    //                 (a user function failure, not a platform failure)
    // "timeout"     - the function did not respond within its configured
    //                 timeout; the runtime is torn down, not reused
    // "infra_error" - the platform could not start or reach a runtime
    //                 for this function (an infrastructure failure)
    std::string status;
    std::string output;  // raw JSON body from the function, or an error message
    long duration_ms = 0;
    bool cold_start = false;
};

struct RuntimeStatus {
    RuntimeState state;
    std::string container_id;
    int host_port;
    long idle_ms;
};

// Owns the mapping from function name to the single running container
// (if any) that serves it: creating one on demand, enforcing the
// function's invocation timeout, retiring runtimes that fail a health
// check or time out, and reaping ones that have sat idle too long.
//
// Container creation happens while holding the manager's single lock,
// which serializes invocations across *all* functions while any one of
// them is cold-starting. That is intentional for now - correctness
// first - and is revisited when concurrency handling is built. One
// consequence: the STARTING state is never observed from outside the
// manager, since a status query would block on the same lock until
// creation finishes.
class RuntimeManager {
public:
    RuntimeManager();
    ~RuntimeManager();
    RuntimeManager(const RuntimeManager&) = delete;
    RuntimeManager& operator=(const RuntimeManager&) = delete;

    InvocationResult invoke(const FunctionSpec& spec, const std::string& input_json);

    // std::nullopt means no runtime is currently running for this
    // function (never invoked yet, or scaled to zero).
    std::optional<RuntimeStatus> status(const std::string& function_name);

private:
    struct Runtime {
        std::string container_id;
        int host_port = 0;
        RuntimeState state = RuntimeState::STARTING;
        std::chrono::steady_clock::time_point last_used_at;
    };

    bool is_healthy(int host_port);
    bool start_runtime(const FunctionSpec& spec, Runtime& out, std::string& error);
    void reap_idle_runtimes();

    std::mutex mutex_;
    std::map<std::string, Runtime> runtimes_;
    DockerClient docker_;

    long idle_timeout_ms_;
    std::atomic<bool> running_{true};
    std::thread reaper_thread_;
};

} // namespace faas
