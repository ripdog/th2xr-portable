#include "message.hpp"

namespace th2 {
namespace {

std::vector<std::string> split_segments(std::string_view source)
{
    std::vector<std::string> segments(1);
    for (std::size_t position = 0; position < source.size();) {
        if (source[position] == '<') {
            const auto end = source.find('>', position + 1);
            if (end != std::string_view::npos && position + 1 < end) {
                const char command = source[position + 1];
                if (command == 's' || command == 'S'
                    || command == 'w' || command == 'W'
                    || command == 'c' || command == 'C'
                    || command == 'f' || command == 'F'
                    || command == 'd' || command == 'D') {
                    position = end + 1;
                    continue;
                }
            }
        }
        if (source[position] == '\\' && position + 1 < source.size()) {
            const char command = source[position + 1];
            position += 2;
            if (command == 'k' || command == 'K') {
                if (segments.back().find_first_not_of(" \t") == std::string::npos) {
                    segments.back().clear();
                }
                segments.emplace_back();
            } else if (command == 'n' || command == 'N') {
                segments.back().push_back('\n');
            }
            continue;
        }
        segments.back().push_back(source[position++]);
    }
    if (segments.size() > 1 && segments.back().empty()) {
        segments.pop_back();
    }
    return segments;
}

}  // namespace

void Message::set(std::string_view source)
{
    segments_ = split_segments(source);
    revealed_ = 0;
    visible_.clear();
    reveal_current();
}

void Message::append(std::string_view source)
{
    auto added = split_segments(source);
    if (segments_.empty()) {
        segments_ = std::move(added);
        revealed_ = 0;
        visible_.clear();
        reveal_current();
        return;
    }
    segments_.insert(
        segments_.end(), std::make_move_iterator(added.begin()),
        std::make_move_iterator(added.end()));
    reveal_current();
}

bool Message::reveal_next()
{
    if (revealed_ >= segments_.size()) {
        return false;
    }
    reveal_current();
    return true;
}

void Message::reveal_current()
{
    if (revealed_ < segments_.size()) {
        visible_ += segments_[revealed_++];
    }
}

void Message::restore_state(
    const std::vector<std::string>& segments,
    std::size_t revealed, const std::string& visible)
{
    segments_ = segments;
    revealed_ = revealed;
    visible_ = visible;
}

}  // namespace th2
