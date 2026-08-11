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
// (if any) that serves it: creating one on demand, reusing a warm one
// without a proactive health check on every call, enforcing the
// function's invocation timeout, retiring runtimes that time out or
// turn out to be dead, and reaping ones that have sat idle too long. A
// warm runtime is trusted until an actual invocation proves otherwise;
// health checks are only used while polling a freshly created container
// for readiness.
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

    // Reuses an existing runtime if there is one, otherwise creates one,
    // then makes the call. Sets connection_failed when the call could
    // not reach the container at all (as opposed to a timeout, which
    // means the container is alive but slow) - that specific failure is
    // what invoke() treats as "the warm runtime turned out to be dead"
    // and retries once against a fresh one.
    InvocationResult try_invoke(const FunctionSpec& spec, const std::string& input_json,
                                 std::chrono::steady_clock::time_point start, bool& connection_failed);

    std::mutex mutex_;
    std::map<std::string, Runtime> runtimes_;
    DockerClient docker_;

    long idle_timeout_ms_;
    std::atomic<bool> running_{true};
    std::thread reaper_thread_;
};

} // namespace faas
