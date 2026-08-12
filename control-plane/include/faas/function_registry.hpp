#pragma once

#include "faas/database.hpp"
#include "faas/function.hpp"

#include <optional>
#include <vector>

namespace faas {

// SQLite-backed store of registered functions, keyed by name. Function
// metadata survives a control-plane restart; runtimes themselves do
// not, since a restarted control plane has no way to know whether a
// container it no longer remembers is still healthy, so it starts fresh
// and lets the next invocation cold-start as usual.
class FunctionRegistry {
public:
    explicit FunctionRegistry(Database& db);

    // Registers a function, replacing any existing function with the same
    // name (this is how re-deploys/version bumps work).
    Function register_function(const FunctionSpec& spec);

    std::optional<Function> get(const std::string& name) const;
    bool remove(const std::string& name);
    std::vector<Function> list() const;

private:
    Database& db_;
};

} // namespace faas
