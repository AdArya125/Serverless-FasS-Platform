#include "faas/database.hpp"

#include <sqlite3.h>

#include <stdexcept>

namespace faas {

Database::Database(const std::string& path) {
    if (sqlite3_open(path.c_str(), &db_) != SQLITE_OK) {
        std::string error = db_ ? sqlite3_errmsg(db_) : "unknown error";
        sqlite3_close(db_);
        throw std::runtime_error("failed to open database at " + path + ": " + error);
    }
    // A peer briefly holding a write lock should be retried, not fail
    // the caller immediately with SQLITE_BUSY.
    sqlite3_busy_timeout(db_, 5000);
}

Database::~Database() {
    if (db_) sqlite3_close(db_);
}

void Database::exec(const std::string& sql) {
    std::lock_guard<std::mutex> lock(mutex_);
    char* error = nullptr;
    if (sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &error) != SQLITE_OK) {
        std::string message = error ? error : "unknown error";
        sqlite3_free(error);
        throw std::runtime_error("sqlite exec failed: " + message);
    }
}

int Database::changes() const { return sqlite3_changes(db_); }

long Database::last_insert_id() const { return static_cast<long>(sqlite3_last_insert_rowid(db_)); }

Statement::Statement(Database& db, const std::string& sql) : lock_(db.mutex()) {
    if (sqlite3_prepare_v2(db.handle(), sql.c_str(), -1, &stmt_, nullptr) != SQLITE_OK) {
        throw std::runtime_error("failed to prepare statement: " + std::string(sqlite3_errmsg(db.handle())));
    }
}

Statement::~Statement() {
    if (stmt_) sqlite3_finalize(stmt_);
}

void Statement::bind(int index, const std::string& value) {
    sqlite3_bind_text(stmt_, index, value.c_str(), -1, SQLITE_TRANSIENT);
}

void Statement::bind(int index, long value) { sqlite3_bind_int64(stmt_, index, value); }

void Statement::bind(int index, double value) { sqlite3_bind_double(stmt_, index, value); }

bool Statement::step() {
    int rc = sqlite3_step(stmt_);
    if (rc == SQLITE_ROW) return true;
    if (rc == SQLITE_DONE) return false;
    throw std::runtime_error("sqlite step failed: " +
                              std::string(sqlite3_errmsg(sqlite3_db_handle(stmt_))));
}

std::string Statement::column_text(int index) const {
    const unsigned char* text = sqlite3_column_text(stmt_, index);
    return text ? reinterpret_cast<const char*>(text) : "";
}

long Statement::column_int64(int index) const { return sqlite3_column_int64(stmt_, index); }

double Statement::column_double(int index) const { return sqlite3_column_double(stmt_, index); }

} // namespace faas
