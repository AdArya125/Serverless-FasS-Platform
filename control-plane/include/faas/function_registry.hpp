#pragma once

#include "faas/function.hpp"

#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

namespace faas {

// In-memory store of registered functions, keyed by name.
//
// This starts out purely in memory; a SQLite-backed implementation is
// added later behind the same interface so callers do not change.
class FunctionRegistry {
public:
    // Registers a function, replacing any existing function with the same
    // name (this is how re-deploys/version bumps work).
    Function register_function(const FunctionSpec& spec);

    std::optional<Function> get(const std::string& name) const;
    bool remove(const std::string& name);
    std::vector<Function> list() const;

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, Function> functions_;
};

} // namespace faas
