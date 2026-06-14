#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace th2 {

class Message {
public:
    void set(std::string_view source);
    void append(std::string_view source);

    bool reveal_next();
    bool has_hidden_segments() const { return revealed_ < segments_.size(); }
    bool empty() const { return segments_.empty(); }
    const std::string& visible() const { return visible_; }
    std::size_t revealed_count() const { return revealed_; }
    const std::vector<std::string>& segments() const { return segments_; }
    void restore_state(const std::vector<std::string>& segments,
                       std::size_t revealed, const std::string& visible);

private:
    std::vector<std::string> segments_;
    std::size_t revealed_ = 0;
    std::string visible_;

    void reveal_current();
};

}  // namespace th2
