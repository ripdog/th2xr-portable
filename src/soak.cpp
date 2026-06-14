#include "soak.hpp"

#include <algorithm>
#include <fstream>
#include <format>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace th2 {
namespace {

std::string encode_path(const std::vector<int>& path)
{
    std::string result;
    for (std::size_t i = 0; i < path.size(); ++i) {
        if (i != 0) {
            result += ',';
        }
        result += std::to_string(path[i]);
    }
    return result;
}

std::vector<int> decode_path(std::string_view text)
{
    std::vector<int> result;
    std::size_t position = 0;
    while (position < text.size()) {
        const auto comma = text.find(',', position);
        const auto token = text.substr(
            position, comma == std::string_view::npos
                ? text.size() - position : comma - position);
        if (token.empty()) {
            throw std::runtime_error("invalid empty soak path component");
        }
        std::size_t consumed = 0;
        const int value = std::stoi(std::string(token), &consumed);
        if (consumed != token.size() || value < 0) {
            throw std::runtime_error("invalid soak path component");
        }
        result.push_back(value);
        if (comma == std::string_view::npos) {
            break;
        }
        position = comma + 1;
    }
    return result;
}

bool contains_path(
    const std::vector<std::vector<int>>& paths, const std::vector<int>& path)
{
    return std::ranges::find(paths, path) != paths.end();
}

std::string kind_name(SoakDecisionKind kind)
{
    return kind == SoakDecisionKind::choice ? "choice" : "map";
}

std::string describe_options(const std::vector<SoakOption>& options)
{
    std::ostringstream output;
    for (std::size_t i = 0; i < options.size(); ++i) {
        if (i != 0) {
            output << "; ";
        }
        output << i << '=' << std::quoted(options[i].label)
               << " -> " << std::quoted(options[i].target);
    }
    return output.str();
}

SoakDecisionKind parse_kind(std::string_view name)
{
    if (name == "choice") {
        return SoakDecisionKind::choice;
    }
    if (name == "map") {
        return SoakDecisionKind::map;
    }
    throw std::runtime_error("invalid soak decision kind");
}

}  // namespace

SoakExplorer::SoakExplorer(
    std::filesystem::path directory, std::size_t run_limit)
    : directory_(std::move(directory)),
      state_path_(directory_ / "state.txt"),
      report_path_(directory_ / "runs.log"),
      run_limit_(run_limit)
{
    std::filesystem::create_directories(directory_);
    load();
    if (pending_.empty() && completed_.empty()) {
        pending_.push_back({});
        save();
    }
}

bool SoakExplorer::begin_run()
{
    if (running_) {
        throw std::logic_error("a soak run is already active");
    }
    if (completed_this_process_ >= run_limit_ || pending_.empty()) {
        return false;
    }
    planned_ = std::move(pending_.front());
    pending_.erase(pending_.begin());
    actual_.clear();
    decision_depth_ = 0;
    running_ = true;
    save();
    return true;
}

int SoakExplorer::select(
    SoakDecisionKind kind, std::string checkpoint,
    const std::vector<SoakOption>& options)
{
    if (!running_) {
        throw std::logic_error("no soak run is active");
    }
    if (options.empty()) {
        throw std::runtime_error(
            "soak encountered a decision with no options at " + checkpoint);
    }

    const auto prefix = encode_path(actual_);
    const auto found = std::ranges::find_if(nodes_, [&](const Node& node) {
        return node.path == prefix;
    });
    if (found == nodes_.end()) {
        nodes_.push_back(Node{
            prefix, kind, checkpoint, options,
        });
    } else if (found->kind != kind || found->checkpoint != checkpoint
               || found->options != options) {
        throw std::runtime_error(std::format(
            "nondeterministic decision at path {}: "
            "recorded {} at {} [{}], observed {} at {} [{}]",
            prefix, kind_name(found->kind), found->checkpoint,
            describe_options(found->options), kind_name(kind), checkpoint,
            describe_options(options)));
    }

    int selected = 0;
    if (decision_depth_ < planned_.size()) {
        selected = planned_[decision_depth_];
        if (selected >= static_cast<int>(options.size())) {
            throw std::runtime_error(
                "recorded soak selection is no longer available at "
                + checkpoint);
        }
    }
    for (int option = 0; option < static_cast<int>(options.size()); ++option) {
        if (option == selected) {
            continue;
        }
        auto sibling = actual_;
        sibling.push_back(option);
        enqueue(std::move(sibling));
    }
    actual_.push_back(selected);
    ++decision_depth_;
    save();
    return selected;
}

void SoakExplorer::complete_run(
    std::string_view terminal, std::string_view script, std::size_t offset)
{
    if (!running_) {
        return;
    }
    if (decision_depth_ < planned_.size()) {
        throw std::runtime_error(
            "soak route terminated before its recorded decision path");
    }
    if (!contains_path(completed_, actual_)) {
        completed_.push_back(actual_);
    }
    pending_.erase(
        std::remove(pending_.begin(), pending_.end(), actual_),
        pending_.end());
    append_report("PASS", terminal, script, offset);
    ++completed_this_process_;
    running_ = false;
    save();
}

void SoakExplorer::fail_run(
    std::string_view error, std::string_view script, std::size_t offset)
{
    if (!running_) {
        return;
    }
    append_report("FAIL", error, script, offset);
    pending_.insert(pending_.begin(), active_path());
    running_ = false;
    save();
}

void SoakExplorer::load()
{
    std::ifstream input(state_path_);
    if (!input) {
        return;
    }
    std::string tag;
    int version = 0;
    input >> tag >> version;
    if (tag != "VERSION" || version != 1) {
        throw std::runtime_error("unsupported soak state version");
    }
    while (input >> tag) {
        if (tag == "PENDING" || tag == "COMPLETED" || tag == "ACTIVE") {
            std::string path;
            input >> std::quoted(path);
            auto decoded = decode_path(path);
            if (tag == "ACTIVE") {
                pending_.insert(pending_.begin(), std::move(decoded));
            } else {
                (tag == "PENDING" ? pending_ : completed_).push_back(
                    std::move(decoded));
            }
        } else if (tag == "NODE") {
            Node node;
            std::string kind;
            std::size_t count = 0;
            input >> std::quoted(node.path) >> kind
                  >> std::quoted(node.checkpoint) >> count;
            node.kind = parse_kind(kind);
            for (std::size_t i = 0; i < count; ++i) {
                SoakOption option;
                input >> std::quoted(option.label)
                      >> std::quoted(option.target);
                node.options.push_back(std::move(option));
            }
            nodes_.push_back(std::move(node));
        } else {
            throw std::runtime_error("invalid soak state record");
        }
    }
}

void SoakExplorer::save() const
{
    const auto temporary = state_path_.string() + ".tmp";
    std::ofstream output(temporary);
    if (!output) {
        throw std::runtime_error("cannot write soak state");
    }
    output << "VERSION 1\n";
    if (running_) {
        output << "ACTIVE " << std::quoted(encode_path(active_path())) << '\n';
    }
    for (const auto& path : pending_) {
        output << "PENDING " << std::quoted(encode_path(path)) << '\n';
    }
    for (const auto& path : completed_) {
        output << "COMPLETED " << std::quoted(encode_path(path)) << '\n';
    }
    for (const auto& node : nodes_) {
        output << "NODE " << std::quoted(node.path) << ' '
               << kind_name(node.kind) << ' '
               << std::quoted(node.checkpoint) << ' '
               << node.options.size();
        for (const auto& option : node.options) {
            output << ' ' << std::quoted(option.label)
                   << ' ' << std::quoted(option.target);
        }
        output << '\n';
    }
    output.close();
    std::filesystem::rename(temporary, state_path_);
}

void SoakExplorer::enqueue(std::vector<int> path)
{
    if (!contains_path(pending_, path) && !contains_path(completed_, path)) {
        pending_.push_back(std::move(path));
    }
}

std::vector<int> SoakExplorer::active_path() const
{
    auto result = planned_;
    if (result.size() < actual_.size()) {
        result.resize(actual_.size());
    }
    std::copy(actual_.begin(), actual_.end(), result.begin());
    return result;
}

void SoakExplorer::append_report(
    std::string_view status, std::string_view detail,
    std::string_view script, std::size_t offset) const
{
    std::ofstream output(report_path_, std::ios::app);
    output << status << " path=" << std::quoted(encode_path(actual_))
           << " decisions=" << actual_.size()
           << " terminal=" << std::quoted(std::string(detail))
           << " position=" << std::quoted(
               std::string(script) + ':' + std::to_string(offset))
           << '\n';
}

}  // namespace th2
