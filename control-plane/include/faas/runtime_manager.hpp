#pragma once

#include "faas/docker_client.hpp"
#include "faas/function.hpp"

#include <map>
#include <mutex>
#include <string>

namespace faas {

struct InvocationResult {
    // "success"     - the function ran and returned a 2xx response
    // "error"       - the function ran but returned a non-2xx response
    //                 (a user function failure, not a platform failure)
    // "infra_error" - the platform could not start or reach a runtime
    //                 for this function (an infrastructure failure)
    std::string status;
    std::string output;  // raw JSON body from the function, or an error message
    long duration_ms = 0;
    bool cold_start = false;
};

// Owns the mapping from function name to the single running container
// (if any) that serves it, and knows how to create one on demand.
//
// This keeps exactly one runtime per function and holds a single lock
// across container creation, which serializes invocations across *all*
// functions while any one of them is cold-starting. That is intentional
// for now - correctness first - and is revisited when concurrency
// handling is built.
class RuntimeManager {
public:
    InvocationResult invoke(const FunctionSpec& spec, const std::string& input_json);

private:
    struct Runtime {
        std::string container_id;
        int host_port = 0;
    };

    bool is_healthy(int host_port);
    bool start_runtime(const FunctionSpec& spec, Runtime& out, std::string& error);

    std::mutex mutex_;
    std::map<std::string, Runtime> runtimes_;
    DockerClient docker_;
};

} // namespace faas
