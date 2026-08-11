#pragma once

#include <string>

namespace faas {

enum class RuntimeState {
    STARTING,
    READY,
    BUSY,
    IDLE,
    FAILED,
    TERMINATED,
};

inline std::string to_string(RuntimeState state) {
    switch (state) {
        case RuntimeState::STARTING: return "STARTING";
        case RuntimeState::READY: return "READY";
        case RuntimeState::BUSY: return "BUSY";
        case RuntimeState::IDLE: return "IDLE";
        case RuntimeState::FAILED: return "FAILED";
        case RuntimeState::TERMINATED: return "TERMINATED";
    }
    return "UNKNOWN";
}

} // namespace faas
