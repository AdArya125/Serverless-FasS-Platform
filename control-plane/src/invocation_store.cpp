#include "faas/invocation_store.hpp"

namespace faas {

namespace {

constexpr const char* kCreateTableSql = R"sql(
CREATE TABLE IF NOT EXISTS invocations (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    function_name TEXT NOT NULL,
    input TEXT NOT NULL,
    output TEXT NOT NULL,
    status TEXT NOT NULL,
    duration_ms INTEGER NOT NULL,
    cold_start INTEGER NOT NULL,
    created_at INTEGER NOT NULL
);
)sql";

} // namespace

InvocationStore::InvocationStore(Database& db) : db_(db) { db_.exec(kCreateTableSql); }

std::string InvocationStore::record(const std::string& function_name, const std::string& input,
                                     const std::string& status, const std::string& output,
                                     long duration_ms, bool cold_start) {
    Statement stmt(db_, R"sql(
        INSERT INTO invocations (function_name, input, output, status, duration_ms, cold_start, created_at)
        VALUES (?, ?, ?, ?, ?, ?, strftime('%s', 'now'))
    )sql");
    stmt.bind(1, function_name);
    stmt.bind(2, input);
    stmt.bind(3, output);
    stmt.bind(4, status);
    stmt.bind(5, duration_ms);
    stmt.bind(6, static_cast<long>(cold_start ? 1 : 0));
    stmt.step();

    return std::to_string(db_.last_insert_id());
}

std::optional<InvocationRecord> InvocationStore::get(const std::string& id) const {
    long numeric_id;
    try {
        numeric_id = std::stol(id);
    } catch (const std::exception&) {
        return std::nullopt; // not a value we could have generated
    }

    Statement stmt(db_,
                    "SELECT id, function_name, input, output, status, duration_ms, cold_start, created_at "
                    "FROM invocations WHERE id = ?");
    stmt.bind(1, numeric_id);
    if (!stmt.step()) return std::nullopt;

    InvocationRecord record;
    record.id = std::to_string(stmt.column_int64(0));
    record.function_name = stmt.column_text(1);
    record.input = stmt.column_text(2);
    record.output = stmt.column_text(3);
    record.status = stmt.column_text(4);
    record.duration_ms = stmt.column_int64(5);
    record.cold_start = stmt.column_int64(6) != 0;
    record.created_at = std::chrono::system_clock::from_time_t(stmt.column_int64(7));
    return record;
}

} // namespace faas
