#include "persistent_state.hpp"

#include <sqlite3.h>

#include <stdexcept>
#include <string>

namespace th2 {
namespace {

class Statement {
public:
    Statement(sqlite3* db, const char* sql) : db_(db) {
        if (sqlite3_prepare_v2(db_, sql, -1, &statement_, nullptr) !=
            SQLITE_OK) {
            throw std::runtime_error(sqlite3_errmsg(db_));
        }
    }

    ~Statement() {
        sqlite3_finalize(statement_);
    }

    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    sqlite3_stmt* get() const {
        return statement_;
    }

    bool step() {
        const int result = sqlite3_step(statement_);
        if (result == SQLITE_ROW) {
            return true;
        }
        if (result == SQLITE_DONE) {
            return false;
        }
        throw std::runtime_error(sqlite3_errmsg(db_));
    }

private:
    sqlite3* db_ = nullptr;
    sqlite3_stmt* statement_ = nullptr;
};

void bind_text(sqlite3* db, sqlite3_stmt* statement, int index,
               std::string_view value) {
    if (sqlite3_bind_text(statement, index, value.data(),
                          static_cast<int>(value.size()),
                          SQLITE_TRANSIENT) != SQLITE_OK) {
        throw std::runtime_error(sqlite3_errmsg(db));
    }
}

void bind_int(sqlite3* db, sqlite3_stmt* statement, int index, int value) {
    if (sqlite3_bind_int(statement, index, value) != SQLITE_OK) {
        throw std::runtime_error(sqlite3_errmsg(db));
    }
}

}  // namespace

PersistentState::PersistentState(const std::filesystem::path& path) {
    reopen(path);
}

PersistentState::~PersistentState() {
    close();
}

void PersistentState::close() {
    sqlite3_close(db_);
    db_ = nullptr;
}

void PersistentState::reopen(const std::filesystem::path& path) {
    close();
    if (sqlite3_open(path.string().c_str(), &db_) != SQLITE_OK) {
        const std::string message =
            db_ ? sqlite3_errmsg(db_) : "could not open state database";
        close();
        throw std::runtime_error(message);
    }
    initialize_schema();
}

void PersistentState::initialize_schema() {
    exec("PRAGMA foreign_keys = ON");
    exec("CREATE TABLE IF NOT EXISTS read_lines ("
         "script TEXT NOT NULL,"
         "position INTEGER NOT NULL,"
         "revealed_count INTEGER NOT NULL,"
         "PRIMARY KEY (script, position, revealed_count))");
    exec("CREATE TABLE IF NOT EXISTS game_flags ("
         "idx INTEGER PRIMARY KEY CHECK (idx >= 0 AND idx < 1024),"
         "value INTEGER NOT NULL)");
    exec("CREATE TABLE IF NOT EXISTS unlocks ("
         "kind TEXT NOT NULL,"
         "id INTEGER NOT NULL,"
         "PRIMARY KEY (kind, id))");
    exec("PRAGMA user_version = 1");
}

void PersistentState::exec(std::string_view sql) const {
    char* error = nullptr;
    if (sqlite3_exec(db_, std::string(sql).c_str(), nullptr, nullptr, &error) !=
        SQLITE_OK) {
        std::string message = error ? error : sqlite3_errmsg(db_);
        sqlite3_free(error);
        throw std::runtime_error(message);
    }
}

bool PersistentState::is_line_read(const ReadMarker& marker) const {
    Statement statement(
        db_, "SELECT 1 FROM read_lines "
             "WHERE script = ? COLLATE NOCASE "
             "AND position = ? AND revealed_count = ?");
    bind_text(db_, statement.get(), 1, marker.script);
    bind_int(db_, statement.get(), 2, static_cast<int>(marker.position));
    bind_int(db_, statement.get(), 3, static_cast<int>(marker.revealed_count));
    return statement.step();
}

bool PersistentState::mark_line_read(const ReadMarker& marker) {
    Statement statement(db_,
                        "INSERT OR IGNORE INTO read_lines "
                        "(script, position, revealed_count) VALUES (?, ?, ?)");
    bind_text(db_, statement.get(), 1, marker.script);
    bind_int(db_, statement.get(), 2, static_cast<int>(marker.position));
    bind_int(db_, statement.get(), 3, static_cast<int>(marker.revealed_count));
    statement.step();
    return sqlite3_changes(db_) > 0;
}

std::array<std::int32_t, 1024> PersistentState::load_game_flags() const {
    std::array<std::int32_t, 1024> flags{};
    Statement statement(db_, "SELECT idx, value FROM game_flags");
    while (statement.step()) {
        const int index = sqlite3_column_int(statement.get(), 0);
        if (index >= 0 && index < static_cast<int>(flags.size())) {
            flags[static_cast<std::size_t>(index)] =
                sqlite3_column_int(statement.get(), 1);
        }
    }
    return flags;
}

void PersistentState::save_game_flags(
    std::span<const std::int32_t, 1024> flags) {
    exec("BEGIN IMMEDIATE");
    try {
        exec("DELETE FROM game_flags");
        Statement statement(
            db_, "INSERT INTO game_flags (idx, value) VALUES (?, ?)");
        for (std::size_t i = 0; i < flags.size(); ++i) {
            if (flags[i] == 0) {
                continue;
            }
            bind_int(db_, statement.get(), 1, static_cast<int>(i));
            bind_int(db_, statement.get(), 2, flags[i]);
            statement.step();
            sqlite3_reset(statement.get());
            sqlite3_clear_bindings(statement.get());
        }
        exec("COMMIT");
    } catch (...) {
        exec("ROLLBACK");
        throw;
    }
}

std::unordered_set<int> PersistentState::load_unlocks(UnlockKind kind) const {
    std::unordered_set<int> unlocks;
    Statement statement(db_, "SELECT id FROM unlocks WHERE kind = ?");
    bind_text(db_, statement.get(), 1, unlock_kind_name(kind));
    while (statement.step()) {
        unlocks.emplace(sqlite3_column_int(statement.get(), 0));
    }
    return unlocks;
}

bool PersistentState::unlock(UnlockKind kind, int id) {
    Statement statement(
        db_, "INSERT OR IGNORE INTO unlocks (kind, id) VALUES (?, ?)");
    bind_text(db_, statement.get(), 1, unlock_kind_name(kind));
    bind_int(db_, statement.get(), 2, id);
    statement.step();
    return sqlite3_changes(db_) > 0;
}

std::string_view PersistentState::unlock_kind_name(UnlockKind kind) {
    switch (kind) {
    case UnlockKind::visual_cg:
        return "visual_cg";
    case UnlockKind::h_cg:
        return "h_cg";
    case UnlockKind::replay:
        return "replay";
    }
    return "";
}

std::optional<ReadMarker> parse_read_marker(std::string_view key) {
    const auto script_end = key.find(':');
    const auto position_end = key.rfind(':');
    if (script_end == std::string_view::npos || position_end == script_end) {
        return std::nullopt;
    }
    try {
        return ReadMarker{
            std::string(key.substr(0, script_end)),
            static_cast<std::uint32_t>(std::stoul(std::string(
                key.substr(script_end + 1, position_end - script_end - 1)))),
            static_cast<std::uint32_t>(
                std::stoul(std::string(key.substr(position_end + 1)))),
        };
    } catch (...) {
        return std::nullopt;
    }
}

}  // namespace th2
