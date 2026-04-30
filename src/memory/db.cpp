#include "memory/db.hpp"

#include "memory/schema.hpp"

#include <sqlite3.h>

#include <utility>

namespace acva::memory {

// (Helper removed — currently every call site builds its own DbError. Kept
// here as a template for the future when prepare/step paths grow richer
// error reporting.)

// ===== Statement =====

Statement::Statement(sqlite3* db, std::string_view sql) {
    const auto rc = sqlite3_prepare_v2(
        db, sql.data(), static_cast<int>(sql.size()), &stmt_, nullptr);
    if (rc != SQLITE_OK) {
        stmt_ = nullptr;
    }
}

Statement::Statement(Statement&& other) noexcept : stmt_(other.stmt_) {
    other.stmt_ = nullptr;
}

Statement& Statement::operator=(Statement&& other) noexcept {
    if (this != &other) {
        if (stmt_) sqlite3_finalize(stmt_);
        stmt_ = other.stmt_;
        other.stmt_ = nullptr;
    }
    return *this;
}

Statement::~Statement() {
    if (stmt_) sqlite3_finalize(stmt_);
}

void Statement::bind(int idx, std::int64_t value) {
    sqlite3_bind_int64(stmt_, idx, value);
}

void Statement::bind(int idx, double value) {
    sqlite3_bind_double(stmt_, idx, value);
}

void Statement::bind(int idx, std::string_view value) {
    sqlite3_bind_text(stmt_, idx, value.data(),
                      static_cast<int>(value.size()), SQLITE_TRANSIENT);
}

void Statement::bind_null(int idx) {
    sqlite3_bind_null(stmt_, idx);
}

void Statement::bind(int idx, const std::optional<std::string>& value) {
    if (value) {
        bind(idx, std::string_view{*value});
    } else {
        bind_null(idx);
    }
}

void Statement::bind(int idx, const std::optional<std::int64_t>& value) {
    if (value) {
        bind(idx, *value);
    } else {
        bind_null(idx);
    }
}

Statement::StepResult Statement::step() {
    const auto rc = sqlite3_step(stmt_);
    if (rc == SQLITE_ROW)  return StepResult::Row;
    if (rc == SQLITE_DONE) return StepResult::Done;
    return StepResult::Error;
}

void Statement::reset() {
    sqlite3_reset(stmt_);
}

void Statement::clear_bindings() {
    sqlite3_clear_bindings(stmt_);
}

std::int64_t Statement::column_int64(int col) const {
    return sqlite3_column_int64(stmt_, col);
}

double Statement::column_double(int col) const {
    return sqlite3_column_double(stmt_, col);
}

std::string Statement::column_text(int col) const {
    const auto* text = sqlite3_column_text(stmt_, col);
    if (!text) return {};
    const auto bytes = sqlite3_column_bytes(stmt_, col);
    return std::string(reinterpret_cast<const char*>(text), static_cast<std::size_t>(bytes));
}

std::optional<std::string> Statement::column_text_opt(int col) const {
    if (column_is_null(col)) return std::nullopt;
    return column_text(col);
}

std::optional<std::int64_t> Statement::column_int64_opt(int col) const {
    if (column_is_null(col)) return std::nullopt;
    return column_int64(col);
}

bool Statement::column_is_null(int col) const {
    return sqlite3_column_type(stmt_, col) == SQLITE_NULL;
}

// ===== Database =====

Result<Database> Database::open(const std::filesystem::path& path) {
    sqlite3* db = nullptr;
    constexpr int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE
                          | SQLITE_OPEN_FULLMUTEX | SQLITE_OPEN_URI;
    const auto rc = sqlite3_open_v2(path.string().c_str(), &db, flags, nullptr);
    if (rc != SQLITE_OK) {
        DbError err{"sqlite open failed: " + std::string(sqlite3_errmsg(db))};
        if (db) sqlite3_close_v2(db);
        return err;
    }

    Database wrapped(db);

    // Apply pragmas. journal_mode=WAL allows concurrent readers while writes
    // happen on the memory thread; synchronous=NORMAL is the WAL recommended
    // setting. foreign_keys is opt-in in SQLite; we want it.
    static constexpr const char* kPragmas =
        "PRAGMA journal_mode=WAL;"
        "PRAGMA synchronous=NORMAL;"
        "PRAGMA foreign_keys=ON;"
        "PRAGMA temp_store=MEMORY;"
        "PRAGMA busy_timeout=5000;";

    if (auto err = wrapped.exec(kPragmas)) {
        return *err;
    }

    // Apply schema. Idempotent (every CREATE uses IF NOT EXISTS).
    if (auto err = wrapped.exec(kSchemaSql)) {
        return *err;
    }

    // Bake the user_version. Migration framework deferred until M3+.
    if (auto err = wrapped.exec("PRAGMA user_version=1;")) {
        return *err;
    }

    return wrapped;
}

Database::Database(Database&& other) noexcept : db_(other.db_) {
    other.db_ = nullptr;
}

Database& Database::operator=(Database&& other) noexcept {
    if (this != &other) {
        if (db_) sqlite3_close_v2(db_);
        db_ = other.db_;
        other.db_ = nullptr;
    }
    return *this;
}

Database::~Database() {
    if (db_) sqlite3_close_v2(db_);
}

std::optional<DbError> Database::exec(std::string_view sql) {
    char* err_msg = nullptr;
    // sqlite3_exec needs null-terminated; copy if needed.
    std::string buf(sql);
    const auto rc = sqlite3_exec(db_, buf.c_str(), nullptr, nullptr, &err_msg);
    if (rc == SQLITE_OK) return std::nullopt;
    DbError out{"sqlite exec: " + std::string(err_msg ? err_msg : "unknown")};
    if (err_msg) sqlite3_free(err_msg);
    return out;
}

std::int64_t Database::last_insert_rowid() const noexcept {
    return sqlite3_last_insert_rowid(db_);
}

// ===== Transaction =====

Database::Transaction::Transaction(Database& db, bool immediate) : db_(db) {
    (void)db_.exec(immediate ? "BEGIN IMMEDIATE;" : "BEGIN;");
}

Database::Transaction::~Transaction() {
    if (active_) {
        (void)db_.exec("ROLLBACK;");
    }
}

std::optional<DbError> Database::Transaction::commit() {
    if (!active_) return std::nullopt;
    auto err = db_.exec("COMMIT;");
    active_ = false;
    return err;
}

void Database::Transaction::rollback() noexcept {
    if (!active_) return;
    (void)db_.exec("ROLLBACK;");
    active_ = false;
}

} // namespace acva::memory
