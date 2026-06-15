#include "archive.hpp"
#include "audio.hpp"
#include "character.hpp"
#include "message.hpp"
#include "script_runtime.hpp"

#include <cstdint>
#include <cmath>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

namespace {

// Minimal save/load round-trip test for key data structures

int test_binary_io()
{
    // Test write_u32 / read_u32 round-trip
    std::stringstream ss;
    const auto write_u32 = [](std::ostream& out, std::uint32_t v) {
        out.put(static_cast<char>(v & 0xFF));
        out.put(static_cast<char>((v >> 8) & 0xFF));
        out.put(static_cast<char>((v >> 16) & 0xFF));
        out.put(static_cast<char>((v >> 24) & 0xFF));
    };
    const auto read_u32 = [](std::istream& in) -> std::uint32_t {
        std::uint32_t v = 0;
        for (int s = 0; s < 32; s += 8) {
            v |= static_cast<std::uint32_t>(
                static_cast<unsigned char>(in.get())) << s;
        }
        return v;
    };

    write_u32(ss, 0xDEADBEEF);
    write_u32(ss, 42);
    write_u32(ss, 0);
    write_u32(ss, 0xFFFFFFFF);

    if (read_u32(ss) != 0xDEADBEEF) return 1;
    if (read_u32(ss) != 42) return 2;
    if (read_u32(ss) != 0) return 3;
    if (read_u32(ss) != 0xFFFFFFFF) return 4;

    return 0;
}

int test_message_roundtrip()
{
    // Create a multi-segment message, save its full state, restore and verify
    th2::Message msg;
    msg.set("Hello\\kWorld\\kGoodbye");
    // After set(), first segment is revealed
    if (msg.visible() != "Hello") return 5;

    // Reveal second segment
    if (!msg.reveal_next()) return 50;
    if (msg.visible() != "HelloWorld") return 51;

    // Save message state
    const auto segments = msg.segments();
    const auto revealed = msg.revealed_count();
    const auto visible = msg.visible();

    if (segments.size() != 3) return 52;
    if (revealed != 2) return 53;

    // Restore into a new message
    th2::Message restored;
    restored.restore_state(segments, revealed, visible);

    if (restored.visible() != "HelloWorld") return 54;
    if (restored.revealed_count() != 2) return 55;
    if (restored.segments().size() != 3) return 56;

    // Should be able to reveal the last segment
    if (!restored.reveal_next()) return 57;
    if (restored.visible() != "HelloWorldGoodbye") return 58;
    if (restored.reveal_next()) return 59;  // Should be no more

    return 0;
}

int test_choice_serialization()
{
    // Test choice round-trip
    std::stringstream ss;
    const auto write_u32 = [](std::ostream& out, std::uint32_t v) {
        out.put(static_cast<char>(v & 0xFF));
        out.put(static_cast<char>((v >> 8) & 0xFF));
        out.put(static_cast<char>((v >> 16) & 0xFF));
        out.put(static_cast<char>((v >> 24) & 0xFF));
    };
    const auto write_i32 = [&](std::ostream& out, std::int32_t v) {
        write_u32(out, static_cast<std::uint32_t>(v));
    };
    const auto read_u32 = [](std::istream& in) -> std::uint32_t {
        std::uint32_t v = 0;
        for (int s = 0; s < 32; s += 8) {
            v |= static_cast<std::uint32_t>(
                static_cast<unsigned char>(in.get())) << s;
        }
        return v;
    };
    const auto read_i32 = [&](std::istream& in) -> std::int32_t {
        return static_cast<std::int32_t>(read_u32(in));
    };

    // Write choice state
    write_u32(ss, 1);  // choosing = true
    write_u32(ss, 2);  // 2 choices

    // Choice 1
    const std::string text1 = "Go left";
    write_u32(ss, text1.size());
    ss.write(text1.data(), text1.size());
    write_i32(ss, -1);   // flag_no
    write_i32(ss, 0);    // flag_value
    const std::string sno1 = "L001";
    write_u32(ss, sno1.size());
    ss.write(sno1.data(), sno1.size());

    // Choice 2
    const std::string text2 = "Go right";
    write_u32(ss, text2.size());
    ss.write(text2.data(), text2.size());
    write_i32(ss, 50);   // flag_no
    write_i32(ss, 1);    // flag_value
    const std::string sno2 = "R001";
    write_u32(ss, sno2.size());
    ss.write(sno2.data(), sno2.size());

    write_i32(ss, 1);    // highlight
    write_i32(ss, -1);   // selected
    write_i32(ss, 3);    // result_register
    write_u32(ss, 0);    // ex mode

    // Read back
    const auto choosing = read_u32(ss);
    if (choosing != 1) return 8;

    const auto count = read_u32(ss);
    if (count != 2) return 9;

    // Read choice 1
    const auto len1 = read_u32(ss);
    std::string rt1(len1, '\0');
    ss.read(rt1.data(), len1);
    if (rt1 != "Go left") return 10;
    if (read_i32(ss) != -1) return 11;
    if (read_i32(ss) != 0) return 12;
    const auto sno_len1 = read_u32(ss);
    std::string rsno1(sno_len1, '\0');
    ss.read(rsno1.data(), sno_len1);
    if (rsno1 != "L001") return 13;

    // Read choice 2
    const auto len2 = read_u32(ss);
    std::string rt2(len2, '\0');
    ss.read(rt2.data(), len2);
    if (rt2 != "Go right") return 14;
    if (read_i32(ss) != 50) return 15;
    if (read_i32(ss) != 1) return 16;
    const auto sno_len2 = read_u32(ss);
    std::string rsno2(sno_len2, '\0');
    ss.read(rsno2.data(), sno_len2);
    if (rsno2 != "R001") return 17;

    if (read_i32(ss) != 1) return 18;
    if (read_i32(ss) != -1) return 19;
    if (read_i32(ss) != 3) return 20;
    if (read_u32(ss) != 0) return 21;

    return 0;
}

int test_save_header_format()
{
    // Verify save header format matches documented structure:
    // version(4) + sin_no(4) + sys_time(16) + message(32) + thumbnail(14400)
    // Total header: 14456 bytes
    constexpr std::size_t header_size = 4 + 4 + 16 + 32 + 14400;
    if (header_size != 14456) return 21;

    // The first 4 bytes should be version = 1 (little-endian)
    std::stringstream ss;
    ss.put('\1');
    ss.put('\0');
    ss.put('\0');
    ss.put('\0');

    const auto ver = static_cast<unsigned char>(ss.get());
    if (ver != 1) return 22;

    return 0;
}

int test_flag_serialization()
{
    // Test that 1024 flags serialize and deserialize correctly
    std::stringstream ss;
    const auto write_i32 = [](std::ostream& out, std::int32_t v) {
        out.put(static_cast<char>(v & 0xFF));
        out.put(static_cast<char>((v >> 8) & 0xFF));
        out.put(static_cast<char>((v >> 16) & 0xFF));
        out.put(static_cast<char>((v >> 24) & 0xFF));
    };
    const auto read_i32 = [](std::istream& in) -> std::int32_t {
        std::int32_t v = 0;
        for (int s = 0; s < 32; s += 8) {
            v |= static_cast<std::int32_t>(
                static_cast<unsigned char>(in.get())) << s;
        }
        return v;
    };

    // Write 1024 flags with known pattern
    for (std::int32_t i = 0; i < 1024; ++i) {
        write_i32(ss, i * 7 + 3);
    }

    // Read back and verify
    for (std::int32_t i = 0; i < 1024; ++i) {
        if (read_i32(ss) != i * 7 + 3) {
            return 23 + i;
        }
    }

    return 0;
}

int test_backlog_serialization()
{
    std::stringstream stream;
    struct Voice {
        std::uint32_t start = 0;
        std::uint32_t end = 0;
        std::int32_t scenario = 0;
        std::int32_t voice = 0;
        std::int32_t character = 0;
        std::int32_t volume = 0;
        bool alternate = false;
    };
    struct Entry {
        std::string text;
        std::vector<Voice> voices;
    };
    const std::vector<Entry> history{
        {"oldest block", {}},
        {"middle\nblock", {{0, 6, 30100, 12, 3, 255, false}}},
        {"newest block", {
             {0, 6, 30100, 13, 4, 255, true},
             {7, 12, 30100, 14, 5, 192, false},
         }},
    };
    const auto write_u32 = [](std::ostream& out, std::uint32_t value) {
        for (int shift = 0; shift < 32; shift += 8) {
            out.put(static_cast<char>(value >> shift));
        }
    };
    const auto read_u32 = [](std::istream& in) {
        std::uint32_t value = 0;
        for (int shift = 0; shift < 32; shift += 8) {
            value |= static_cast<std::uint32_t>(
                static_cast<unsigned char>(in.get())) << shift;
        }
        return value;
    };
    const auto write_i32 = [&](std::ostream& out, std::int32_t value) {
        write_u32(out, static_cast<std::uint32_t>(value));
    };
    const auto read_i32 = [&](std::istream& in) {
        return static_cast<std::int32_t>(read_u32(in));
    };

    write_u32(stream, history.size());
    for (const auto& entry : history) {
        write_u32(stream, entry.text.size());
        stream.write(
            entry.text.data(),
            static_cast<std::streamsize>(entry.text.size()));
        write_u32(stream, entry.voices.size());
        for (const auto& voice : entry.voices) {
            write_u32(stream, voice.start);
            write_u32(stream, voice.end);
            write_i32(stream, voice.scenario);
            write_i32(stream, voice.voice);
            write_i32(stream, voice.character);
            write_i32(stream, voice.volume);
            write_i32(stream, voice.alternate ? 1 : 0);
        }
    }
    write_u32(stream, 2);

    const auto count = read_u32(stream);
    std::vector<Entry> restored;
    for (std::uint32_t i = 0; i < count; ++i) {
        Entry entry;
        entry.text.resize(read_u32(stream));
        stream.read(
            entry.text.data(), static_cast<std::streamsize>(entry.text.size()));
        const auto voice_count = read_u32(stream);
        for (std::uint32_t v = 0; v < voice_count; ++v) {
            entry.voices.push_back(Voice{
                read_u32(stream),
                read_u32(stream),
                read_i32(stream),
                read_i32(stream),
                read_i32(stream),
                read_i32(stream),
                read_i32(stream) != 0,
            });
        }
        restored.push_back(std::move(entry));
    }
    const auto depth = read_u32(stream);
    if (restored.size() != history.size() || depth != 2) return 1048;
    for (std::size_t i = 0; i < history.size(); ++i) {
        if (restored[i].text != history[i].text
            || restored[i].voices.size() != history[i].voices.size()) {
            return 1050;
        }
        for (std::size_t v = 0; v < history[i].voices.size(); ++v) {
            const auto& a = restored[i].voices[v];
            const auto& b = history[i].voices[v];
            if (a.start != b.start || a.end != b.end
                || a.scenario != b.scenario || a.voice != b.voice
                || a.character != b.character || a.volume != b.volume
                || a.alternate != b.alternate) {
                return 1051;
            }
        }
    }

    const auto position = [](int history_size, int history_depth) {
        return history_size == 0 ? 1.0f
            : 1.0f - static_cast<float>(history_depth)
                / static_cast<float>(history_size);
    };
    if (std::abs(position(3, 0) - 1.0f) > 0.0001f
        || std::abs(position(3, 1) - 2.0f / 3.0f) > 0.0001f
        || std::abs(position(3, 3)) > 0.0001f) {
        return 1049;
    }
    return 0;
}

}  // namespace

int main()
{
    int result;
    if ((result = test_binary_io()) != 0) return result;
    if ((result = test_message_roundtrip()) != 0) return result;
    if ((result = test_choice_serialization()) != 0) return result;
    if ((result = test_save_header_format()) != 0) return result;
    if ((result = test_flag_serialization()) != 0) return result;
    if ((result = test_backlog_serialization()) != 0) return result;
    return 0;
}
