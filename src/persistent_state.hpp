#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>

struct sqlite3;

namespace th2 {

struct ReadMarker {
    std::string script;
    std::uint32_t position = 0;
    std::uint32_t revealed_count = 0;
};

class PersistentState {
public:
    enum class UnlockKind {
        visual_cg,
        h_cg,
        replay,
    };

    explicit PersistentState(const std::filesystem::path& path);
    ~PersistentState();

    PersistentState(const PersistentState&) = delete;
    PersistentState& operator=(const PersistentState&) = delete;

    void close();
    void reopen(const std::filesystem::path& path);

    bool is_line_read(const ReadMarker& marker) const;
    bool mark_line_read(const ReadMarker& marker);

    std::array<std::int32_t, 1024> load_game_flags() const;
    void save_game_flags(std::span<const std::int32_t, 1024> flags);

    std::unordered_set<int> load_unlocks(UnlockKind kind) const;
    bool unlock(UnlockKind kind, int id);

private:
    sqlite3* db_ = nullptr;

    void initialize_schema();
    void exec(std::string_view sql) const;
    static std::string_view unlock_kind_name(UnlockKind kind);
};

std::optional<ReadMarker> parse_read_marker(std::string_view key);

}  // namespace th2
