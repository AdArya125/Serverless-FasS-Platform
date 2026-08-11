#pragma once

#include <chrono>
#include <string>

namespace faas {

enum class FunctionStatus {
    READY,
};

inline std::string to_string(FunctionStatus status) {
    switch (status) {
        case FunctionStatus::READY: return "READY";
    }
    return "UNKNOWN";
}

// The declared shape of a function: what image runs it and the execution
// limits it must respect. This is what a client sends to POST /functions.
struct FunctionSpec {
    std::string name;
    int version = 1;
    std::string image;
    int timeout_ms = 5000;
    int memory_mb = 256;
    double cpu = 0.5;
    int max_concurrency = 1;
};

// A registered function as tracked by the control plane.
struct Function {
    FunctionSpec spec;
    FunctionStatus status = FunctionStatus::READY;
    std::chrono::system_clock::time_point created_at;
};

} // namespace faas
