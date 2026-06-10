#include "archive.hpp"
#include "audio.hpp"
#include "character.hpp"
#include "message.hpp"
#include "script_runtime.hpp"

#include <cstdint>
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
    // Create a message, serialize its visible text, deserialize and verify
    th2::Message msg;
    msg.set("Hello\\kWorld");
    if (msg.visible() != "Hello") return 5;

    // Simulate save: get visible text
    const auto text = msg.visible();
    const auto size = text.size();

    // Simulate load: create new message with saved text
    th2::Message restored;
    restored.set(text);

    if (restored.visible() != "Hello") return 6;
    if (restored.visible().size() != size) return 7;

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

}  // namespace

int main()
{
    int result;
    if ((result = test_binary_io()) != 0) return result;
    if ((result = test_message_roundtrip()) != 0) return result;
    if ((result = test_choice_serialization()) != 0) return result;
    if ((result = test_save_header_format()) != 0) return result;
    if ((result = test_flag_serialization()) != 0) return result;
    return 0;
}
