#include "persistent_state.hpp"

#include <filesystem>

int main() {
    const auto path =
        std::filesystem::temp_directory_path() / "th2-state-test.sqlite3";
    std::filesystem::remove(path);

    {
        th2::PersistentState state(path);
        const th2::ReadMarker marker{"EV_0301MORNING.SDT", 42, 17};
        if (state.is_line_read(marker)) {
            return 1;
        }
        if (!state.mark_line_read(marker) || state.mark_line_read(marker)) {
            return 2;
        }
        if (!state.is_line_read(marker)) {
            return 3;
        }
        if (!state.is_line_read({"ev_0301morning.sdt", 42, 17})) {
            return 7;
        }

        std::array<std::int32_t, 1024> flags{};
        flags[80] = 7;
        flags[128] = 1;
        state.save_game_flags(flags);

        if (!state.unlock(th2::PersistentState::UnlockKind::visual_cg, 1010) ||
            state.unlock(th2::PersistentState::UnlockKind::visual_cg, 1010) ||
            !state.unlock(th2::PersistentState::UnlockKind::h_cg, 1000) ||
            !state.unlock(th2::PersistentState::UnlockKind::replay, 7)) {
            return 4;
        }
    }

    {
        th2::PersistentState state(path);
        const th2::ReadMarker marker{"EV_0301MORNING.SDT", 42, 17};
        const auto flags = state.load_game_flags();
        if (!state.is_line_read(marker) || flags[80] != 7 || flags[128] != 1) {
            return 5;
        }
        if (!state.load_unlocks(th2::PersistentState::UnlockKind::visual_cg)
                 .contains(1010) ||
            !state.load_unlocks(th2::PersistentState::UnlockKind::h_cg)
                 .contains(1000) ||
            !state.load_unlocks(th2::PersistentState::UnlockKind::replay)
                 .contains(7)) {
            return 6;
        }
    }

    std::filesystem::remove(path);
    return 0;
}
