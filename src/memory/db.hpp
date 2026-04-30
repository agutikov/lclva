#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

struct sqlite3;
struct sqlite3_stmt;

namespace lclva::memory {

struct DbError {
    std::string message;
};

template <class T>
using Result = std::variant<T, DbError>;

// Thin RAII over a prepared statement.
//
// Bind 1-indexed; step() returns kRow / kDone / kError. Column accessors are
// 0-indexed. The Statement lives as long as its owner Database; resetting it
// returns it to the pre-step state for reuse.
class Statement {
public:
    enum class StepResult { Row, Done, Error };

    Statement() = default;
    Statement(sqlite3* db, std::string_view sql);
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;
    Statement(Statement&& other) noexcept;
    Statement& operator=(Statement&& other) noexcept;
    ~Statement();

    [[nodiscard]] bool ok() const noexcept { return stmt_ != nullptr; }
    [[nodiscard]] sqlite3_stmt* raw() noexcept { return stmt_; }

    void bind(int idx, std::int64_t value);
    void bind(int idx, double value);
    void bind(int idx, std::string_view value);
    void bind_null(int idx);

    // Bind to a 1-indexed parameter, treating nullopt as NULL.
    void bind(int idx, const std::optional<std::string>& value);
    void bind(int idx, const std::optional<std::int64_t>& value);

    StepResult step();
    void reset();
    void clear_bindings();

    [[nodiscard]] std::int64_t column_int64(int col) const;
    [[nodiscard]] double column_double(int col) const;
    [[nodiscard]] std::string column_text(int col) const;
    [[nodiscard]] std::optional<std::string> column_text_opt(int col) const;
    [[nodiscard]] std::optional<std::int64_t> column_int64_opt(int col) const;
    [[nodiscard]] bool column_is_null(int col) const;

private:
    sqlite3_stmt* stmt_ = nullptr;
};

// RAII over sqlite3*. Opens with WAL + synchronous=NORMAL + foreign_keys=ON.
//
// Single-thread access expected — the MemoryThread owns the only Database
// instance and calls into it from one writer thread. Other threads must go
// through the MemoryThread.
class Database {
public:
    Database() = default;

    // Open or create a SQLite database. Applies pragmas, runs schema, and
    // sets user_version=1.
    [[nodiscard]] static Result<Database> open(const std::filesystem::path& path);

    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;
    Database(Database&& other) noexcept;
    Database& operator=(Database&& other) noexcept;
    ~Database();

    [[nodiscard]] sqlite3* raw() noexcept { return db_; }
    [[nodiscard]] bool ok() const noexcept { return db_ != nullptr; }

    // Run a single SQL statement that doesn't return rows. Returns DbError on
    // failure (typo, constraint, etc.).
    [[nodiscard]] std::optional<DbError> exec(std::string_view sql);

    // Last sqlite_last_insert_rowid().
    [[nodiscard]] std::int64_t last_insert_rowid() const noexcept;

    // Transaction guard. Begin on construction, commit on commit() or
    // destructor; rollback on rollback() or if commit was never called.
    class Transaction {
    public:
        Transaction(Database& db, bool immediate);
        ~Transaction();
        Transaction(const Transaction&) = delete;
        Transaction& operator=(const Transaction&) = delete;

        [[nodiscard]] std::optional<DbError> commit();
        void rollback() noexcept;

    private:
        Database& db_;
        bool active_ = true;
    };

private:
    explicit Database(sqlite3* db) : db_(db) {}
    sqlite3* db_ = nullptr;
};

} // namespace lclva::memory
