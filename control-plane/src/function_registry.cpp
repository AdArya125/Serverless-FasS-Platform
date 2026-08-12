#include "faas/function_registry.hpp"

namespace faas {

namespace {

constexpr const char* kCreateTableSql = R"sql(
CREATE TABLE IF NOT EXISTS functions (
    name TEXT PRIMARY KEY,
    version INTEGER NOT NULL,
    image TEXT NOT NULL,
    timeout_ms INTEGER NOT NULL,
    memory_mb INTEGER NOT NULL,
    cpu REAL NOT NULL,
    max_concurrency INTEGER NOT NULL,
    idle_timeout_ms INTEGER NOT NULL,
    status TEXT NOT NULL,
    created_at INTEGER NOT NULL
);
)sql";

constexpr const char* kSelectColumns =
    "name, version, image, timeout_ms, memory_mb, cpu, max_concurrency, idle_timeout_ms, status, created_at";

Function row_to_function(const Statement& stmt) {
    Function fn;
    fn.spec.name = stmt.column_text(0);
    fn.spec.version = static_cast<int>(stmt.column_int64(1));
    fn.spec.image = stmt.column_text(2);
    fn.spec.timeout_ms = static_cast<int>(stmt.column_int64(3));
    fn.spec.memory_mb = static_cast<int>(stmt.column_int64(4));
    fn.spec.cpu = stmt.column_double(5);
    fn.spec.max_concurrency = static_cast<int>(stmt.column_int64(6));
    fn.spec.idle_timeout_ms = stmt.column_int64(7);
    fn.status = FunctionStatus::READY; // the only status currently defined
    fn.created_at = std::chrono::system_clock::from_time_t(stmt.column_int64(9));
    return fn;
}

} // namespace

FunctionRegistry::FunctionRegistry(Database& db) : db_(db) { db_.exec(kCreateTableSql); }

Function FunctionRegistry::register_function(const FunctionSpec& spec) {
    {
        Statement stmt(db_, R"sql(
            INSERT INTO functions (name, version, image, timeout_ms, memory_mb, cpu, max_concurrency, idle_timeout_ms, status, created_at)
            VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, strftime('%s', 'now'))
            ON CONFLICT(name) DO UPDATE SET
                version = excluded.version,
                image = excluded.image,
                timeout_ms = excluded.timeout_ms,
                memory_mb = excluded.memory_mb,
                cpu = excluded.cpu,
                max_concurrency = excluded.max_concurrency,
                idle_timeout_ms = excluded.idle_timeout_ms,
                status = excluded.status,
                created_at = strftime('%s', 'now')
        )sql");
        stmt.bind(1, spec.name);
        stmt.bind(2, static_cast<long>(spec.version));
        stmt.bind(3, spec.image);
        stmt.bind(4, static_cast<long>(spec.timeout_ms));
        stmt.bind(5, static_cast<long>(spec.memory_mb));
        stmt.bind(6, spec.cpu);
        stmt.bind(7, static_cast<long>(spec.max_concurrency));
        stmt.bind(8, spec.idle_timeout_ms);
        stmt.bind(9, to_string(FunctionStatus::READY));
        stmt.step();
    } // stmt's lock is released here, before get() takes its own

    return *get(spec.name);
}

std::optional<Function> FunctionRegistry::get(const std::string& name) const {
    Statement stmt(db_, std::string("SELECT ") + kSelectColumns + " FROM functions WHERE name = ?");
    stmt.bind(1, name);
    if (!stmt.step()) return std::nullopt;
    return row_to_function(stmt);
}

bool FunctionRegistry::remove(const std::string& name) {
    Statement stmt(db_, "DELETE FROM functions WHERE name = ?");
    stmt.bind(1, name);
    stmt.step();
    return db_.changes() > 0;
}

std::vector<Function> FunctionRegistry::list() const {
    Statement stmt(db_, std::string("SELECT ") + kSelectColumns + " FROM functions");
    std::vector<Function> result;
    while (stmt.step()) {
        result.push_back(row_to_function(stmt));
    }
    return result;
}

} // namespace faas
