#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace th2 {

enum class SoakDecisionKind {
    choice,
    map,
};

struct SoakOption {
    std::string label;
    std::string target;

    bool operator==(const SoakOption&) const = default;
};

class SoakExplorer {
public:
    SoakExplorer(std::filesystem::path directory, std::size_t run_limit);

    bool begin_run();
    int select(SoakDecisionKind kind, std::string checkpoint,
               const std::vector<SoakOption>& options);
    void complete_run(std::string_view terminal, std::string_view script,
                      std::size_t offset);
    void fail_run(std::string_view error, std::string_view script,
                  std::size_t offset);

    std::size_t completed_runs() const { return completed_this_process_; }
    std::size_t pending_paths() const { return pending_.size(); }
    const std::filesystem::path& directory() const { return directory_; }

private:
    struct Node {
        std::string path;
        SoakDecisionKind kind = SoakDecisionKind::choice;
        std::string checkpoint;
        std::vector<SoakOption> options;
    };

    std::filesystem::path directory_;
    std::filesystem::path state_path_;
    std::filesystem::path report_path_;
    std::size_t run_limit_;
    std::size_t completed_this_process_ = 0;
    std::vector<std::vector<int>> pending_;
    std::vector<std::vector<int>> completed_;
    std::vector<Node> nodes_;
    std::vector<int> planned_;
    std::vector<int> actual_;
    std::size_t decision_depth_ = 0;
    bool running_ = false;

    void load();
    void save() const;
    void enqueue(std::vector<int> path);
    std::vector<int> active_path() const;
    void append_report(std::string_view status, std::string_view detail,
                       std::string_view script, std::size_t offset) const;
};

}  // namespace th2
