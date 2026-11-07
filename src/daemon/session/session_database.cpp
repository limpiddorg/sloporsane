#include "session_database.hpp"
#include <sqlite3.h>
#include <stdexcept>
#include <limits>

namespace {
void check(int status, sqlite3* db) {
    if (status != SQLITE_OK) throw std::runtime_error(std::string("Session database: ") + sqlite3_errmsg(db));
}
}

SessionDatabase::SessionDatabase(const std::filesystem::path& path) {
    const auto status = sqlite3_open_v2(path.c_str(), &connection,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr);
    try {
        check(status, connection);
        check(sqlite3_busy_timeout(connection, 5000), connection);
        execute("PRAGMA foreign_keys=ON; PRAGMA trusted_schema=OFF; PRAGMA cache_size=-4096;"
                "PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL;");
    } catch (...) { sqlite3_close(connection); connection = nullptr; throw; }
}
SessionDatabase::~SessionDatabase() { sqlite3_close(connection); }
void SessionDatabase::execute(const char* sql) { check(sqlite3_exec(connection, sql, nullptr, nullptr, nullptr), connection); }
SessionDatabase::Statement::Statement(SessionDatabase& db, const char* sql) {
    check(sqlite3_prepare_v2(db.handle(), sql, -1, &statement, nullptr), db.handle());
}
SessionDatabase::Statement::~Statement() { sqlite3_finalize(statement); }
void SessionDatabase::Statement::bind(int index, std::int64_t value) {
    check(sqlite3_bind_int64(statement, index, value), sqlite3_db_handle(statement));
}
void SessionDatabase::Statement::bind(int index, std::string_view value) {
    if (value.size() > std::numeric_limits<int>::max()) throw std::length_error("Session value exceeds SQLite limit");
    check(sqlite3_bind_text(statement, index, value.data(), static_cast<int>(value.size()), SQLITE_TRANSIENT), sqlite3_db_handle(statement));
}
bool SessionDatabase::Statement::step() {
    const auto status = sqlite3_step(statement);
    if (status == SQLITE_ROW) return true;
    if (status == SQLITE_DONE) return false;
    check(status, sqlite3_db_handle(statement));
    return false;
}
std::int64_t SessionDatabase::Statement::integer(int column) const { return sqlite3_column_int64(statement, column); }
std::string SessionDatabase::Statement::text(int column) const {
    const auto* value = sqlite3_column_text(statement, column);
    return value ? std::string(reinterpret_cast<const char*>(value), sqlite3_column_bytes(statement, column)) : std::string{};
}
SessionDatabase::Transaction::Transaction(SessionDatabase& db) : db(db) { db.execute("BEGIN IMMEDIATE"); }
SessionDatabase::Transaction::~Transaction() { if (!committed) try { db.execute("ROLLBACK"); } catch (...) {} }
void SessionDatabase::Transaction::commit() { db.execute("COMMIT"); committed = true; }
