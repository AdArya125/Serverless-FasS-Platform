#pragma once
// Thin, thread-safe wrapper around a single SQLite connection, used to
// persist function metadata and invocation history so both survive a
// control-plane restart.

#include <mutex>
#include <string>

extern "C" {
struct sqlite3;
struct sqlite3_stmt;
}

namespace faas {

class Database {
public:
    explicit Database(const std::string& path);
    ~Database();
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    // Runs a statement with no parameters and no result rows, e.g. a
    // schema migration.
    void exec(const std::string& sql);

    int changes() const;      // rows affected by the most recent statement
    long last_insert_id() const;

    sqlite3* handle() const { return db_; }
    std::mutex& mutex() { return mutex_; }

private:
    sqlite3* db_ = nullptr;
    std::mutex mutex_;
};

// A prepared statement bound to a Database. Holds that database's lock
// for its entire lifetime, from prepare through the last step, so two
// threads can never interleave binds/steps on the same connection.
class Statement {
public:
    Statement(Database& db, const std::string& sql);
    ~Statement();
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    void bind(int index, const std::string& value);
    void bind(int index, long value);
    void bind(int index, double value);

    // True if a row is available; false once the statement is exhausted.
    bool step();

    std::string column_text(int index) const;
    long column_int64(int index) const;
    double column_double(int index) const;

private:
    std::unique_lock<std::mutex> lock_;
    sqlite3_stmt* stmt_ = nullptr;
};

} // namespace faas
