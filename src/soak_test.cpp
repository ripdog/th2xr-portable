#include "soak.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {

bool expect(bool condition, std::string_view message)
{
    if (!condition) {
        std::cerr << message << '\n';
        return false;
    }
    return true;
}

}  // namespace

int main()
{
    const auto directory =
        std::filesystem::temp_directory_path() / "th2-soak-test";
    std::filesystem::remove_all(directory);

    {
        th2::SoakExplorer explorer(directory, 2);
        if (!expect(explorer.begin_run(), "initial route was not queued")) {
            return 1;
        }
        const std::vector options{
            th2::SoakOption{"first", "A"},
            th2::SoakOption{"second", "B"},
            th2::SoakOption{"third", "C"},
        };
        if (!expect(
                explorer.select(
                    th2::SoakDecisionKind::choice, "test.sdt:10", options) == 0,
                "initial route did not choose the first option")) {
            return 1;
        }
        explorer.complete_run("title", "test.sdt", 20);
        if (!expect(explorer.pending_paths() == 2, "siblings were not queued")) {
            return 1;
        }
        if (!expect(explorer.begin_run(), "second route was not queued")) {
            return 1;
        }
        if (!expect(
                explorer.select(
                    th2::SoakDecisionKind::choice, "test.sdt:10", options) == 1,
                "queued sibling was not replayed")) {
            return 1;
        }
        explorer.complete_run("title", "test.sdt", 20);
    }

    {
        th2::SoakExplorer explorer(directory, 10);
        if (!expect(explorer.pending_paths() == 1, "state did not persist")) {
            return 1;
        }
        if (!expect(explorer.begin_run(), "persisted route was not available")) {
            return 1;
        }
        const std::vector options{
            th2::SoakOption{"first", "A"},
            th2::SoakOption{"second", "B"},
            th2::SoakOption{"third", "C"},
        };
        if (!expect(
                explorer.select(
                    th2::SoakDecisionKind::choice, "test.sdt:10", options) == 2,
                "persisted sibling selected the wrong option")) {
            return 1;
        }
        explorer.complete_run("title", "test.sdt", 20);
        if (!expect(explorer.pending_paths() == 0, "exploration did not finish")) {
            return 1;
        }
    }

    std::filesystem::remove_all(directory);

    const auto recovery_directory =
        std::filesystem::temp_directory_path() / "th2-soak-recovery-test";
    std::filesystem::remove_all(recovery_directory);
    {
        th2::SoakExplorer explorer(recovery_directory, 1);
        if (!expect(explorer.begin_run(), "recovery route was not queued")) {
            return 1;
        }
        const std::vector options{
            th2::SoakOption{"first", "A"},
            th2::SoakOption{"second", "B"},
        };
        explorer.select(
            th2::SoakDecisionKind::choice, "recovery.sdt:10", options);
    }
    {
        th2::SoakExplorer explorer(recovery_directory, 1);
        if (!expect(
                explorer.pending_paths() == 2,
                "active route was not recovered alongside its sibling")) {
            return 1;
        }
    }
    std::filesystem::remove_all(recovery_directory);
    return 0;
}
