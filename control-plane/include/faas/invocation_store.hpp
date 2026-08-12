#pragma once

#include "faas/database.hpp"

#include <chrono>
#include <optional>
#include <string>

namespace faas {

struct InvocationRecord {
    std::string id;
    std::string function_name;
    std::string input;
    std::string output;
    std::string status;
    long duration_ms = 0;
    bool cold_start = false;
    std::chrono::system_clock::time_point created_at;
};

// SQLite-backed history of invocations, so past results and timing
// survive a control-plane restart and can be looked up by id.
class InvocationStore {
public:
    explicit InvocationStore(Database& db);

    // Records the outcome of an invocation and returns its generated id.
    std::string record(const std::string& function_name, const std::string& input,
                        const std::string& status, const std::string& output, long duration_ms,
                        bool cold_start);

    std::optional<InvocationRecord> get(const std::string& id) const;

private:
    Database& db_;
};

} // namespace faas
