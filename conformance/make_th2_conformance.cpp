#include "../native/event_metadata.h"

#include <array>
#include <cstdint>
#include <fstream>
#include <initializer_list>
#include <map>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

using Arg = std::variant<int, std::string>;

class Builder {
public:
    void u8(int value) { code_.push_back(static_cast<std::uint8_t>(value)); }
    void u16(int value)
    {
        u8(value);
        u8(value >> 8);
    }
    void u32(std::uint32_t value)
    {
        u16(value);
        u16(value >> 16);
    }
    void number(int value)
    {
        u8(1);
        u32(static_cast<std::uint32_t>(value));
    }
    void reg_number(int index)
    {
        u8(0);
        u32(static_cast<std::uint32_t>(index));
    }
    void core(int opcode) { u16(opcode); }
    void movv(int reg, int value) { core(3); u8(reg); u32(value); }
    void movr(int dst, int src) { core(2); u8(dst); u8(src); }
    void swap(int a, int b) { core(4); u8(a); u8(b); }
    void rand(int reg) { core(5); u8(reg); }
    void unary(int opcode, int reg) { core(opcode); u8(reg); }
    void binary_v(int opcode, int reg, int value)
    {
        core(opcode);
        u8(reg);
        u32(value);
    }
    void binary_r(int opcode, int reg, int source)
    {
        core(opcode);
        u8(reg);
        u8(source);
    }
    void wait(int frames) { core(37); u16(frames); }
    void run() { core(39); }
    void end() { core(1); }

    // Calculation expression factors (postfix).
    static std::vector<std::uint8_t> calc_imm(int value)
    {
        std::vector<std::uint8_t> result{0};
        result.push_back(static_cast<std::uint8_t>(value));
        result.push_back(static_cast<std::uint8_t>(value >> 8));
        result.push_back(static_cast<std::uint8_t>(value >> 16));
        result.push_back(static_cast<std::uint8_t>(value >> 24));
        return result;
    }
    static std::vector<std::uint8_t> calc_reg(int reg)
    {
        return {1, static_cast<std::uint8_t>(reg)};
    }
    static std::vector<std::uint8_t> calc_op(int op)
    {
        return {2, static_cast<std::uint8_t>(op)};
    }
    void calc(int destination, const std::vector<std::uint8_t>& expression)
    {
        core(32);
        u8(destination);
        u16(static_cast<int>(expression.size()));
        code_.insert(code_.end(), expression.begin(), expression.end());
    }

    void label(const std::string& name)
    {
        if (!labels_.emplace(name, code_.size()).second) {
            throw std::runtime_error("duplicate label " + name);
        }
    }
    void jump(const std::string& target)
    {
        core(11);
        patch(target);
    }
    void ifv(int reg, int comparison, int value, const std::string& target)
    {
        core(7);
        u8(reg);
        u8(comparison);
        u32(value);
        patch(target);
    }
    void ifr(int reg, int comparison, int right, const std::string& target)
    {
        core(6);
        u8(reg);
        u8(comparison);
        u8(right);
        patch(target);
    }
    void loop(int reg, const std::string& target)
    {
        core(10);
        u8(reg);
        patch(target);
    }

    void compare_test(int reg, int comparison, int value, const std::string& name)
    {
        const auto pass = "cmp_pass_" + name;
        const auto done = "cmp_done_" + name;
        ifv(reg, comparison, value, pass);
        message("FAIL: " + name);
        jump(done);
        label(pass);
        message("PASS: " + name);
        label(done);
    }

    void event(const std::string& name, std::initializer_list<Arg> args)
    {
        int opcode = -1;
        for (unsigned candidate = 64;
             candidate < 64 + th2_event_count(); ++candidate) {
            if (const char* event_name = th2_event_name(candidate);
                event_name && name == event_name) {
                opcode = static_cast<int>(candidate);
                break;
            }
        }
        if (opcode < 0) throw std::runtime_error("unknown event " + name);
        const char* types = th2_event_parameters(opcode);
        if (!types) throw std::runtime_error("missing metadata " + name);
        u16(opcode);
        auto argument = args.begin();
        for (int i = 0; i < 15 && types[i]; ++i, ++argument) {
            if (argument == args.end()) {
                throw std::runtime_error("too few arguments for " + name);
            }
            const int type = static_cast<unsigned char>(types[i]);
            if (type == TH2_PARAMETER_STRING8
                || type == TH2_PARAMETER_STRING16) {
                const auto* value = std::get_if<std::string>(&*argument);
                if (!value) throw std::runtime_error("expected string for " + name);
                if (type == TH2_PARAMETER_STRING8) {
                    if (value->size() > 255) throw std::runtime_error("string8 overflow");
                    u8(value->size());
                } else {
                    u16(value->size());
                }
                code_.insert(code_.end(), value->begin(), value->end());
            } else {
                const auto* value = std::get_if<int>(&*argument);
                if (!value) throw std::runtime_error("expected number for " + name);
                switch (type) {
                case TH2_PARAMETER_BYTE:
                case TH2_PARAMETER_ADD:
                case TH2_PARAMETER_REGISTER:
                    u8(*value);
                    break;
                case TH2_PARAMETER_COUNT:
                case TH2_PARAMETER_VOICE_COUNT:
                    u16(*value);
                    break;
                case TH2_PARAMETER_NUMBER:
                    number(*value);
                    break;
                case TH2_PARAMETER_COMPARE:
                    throw std::runtime_error("COMPARE unused in retail suite");
                default:
                    throw std::runtime_error("bad parameter metadata");
                }
            }
        }
        if (argument != args.end()) {
            throw std::runtime_error("too many arguments for " + name);
        }
    }

    void message(const std::string& text)
    {
        ++case_;
        event("SetMessage2", {
            (case_ < 10 ? "0" : "") + std::to_string(case_) + " " + text,
            2, 1
        });
    }
    void append(const std::string& text) { event("AddMessage2", {text, 2}); }

    void check(int reg, int expected, const std::string& what)
    {
        const auto pass = "pass_" + std::to_string(case_) + "_" + what;
        const auto done = "done_" + std::to_string(case_) + "_" + what;
        ifv(reg, 4, expected, pass);
        message("FAIL: " + what);
        jump(done);
        label(pass);
        message("PASS: " + what);
        label(done);
    }

    void check_flag(int flag, int expected, const std::string& what)
    {
        event("GetFlag", {flag, 15});
        check(15, expected, what);
    }

    std::vector<std::uint8_t> finish()
    {
        for (const auto& [position, name] : patches_) {
            const auto found = labels_.find(name);
            if (found == labels_.end()) throw std::runtime_error("missing label " + name);
            const auto value = static_cast<std::uint32_t>(found->second);
            for (int i = 0; i < 4; ++i) {
                code_[position + i] = static_cast<std::uint8_t>(value >> (i * 8));
            }
        }
        std::vector<std::uint8_t> file(1032, 0);
        file[0] = 'L';
        file[2] = 'F';
        const auto size = static_cast<std::uint32_t>(file.size() + code_.size());
        for (int i = 0; i < 4; ++i) file[4 + i] = size >> (i * 8);
        file[8] = 1;
        file.insert(file.end(), code_.begin(), code_.end());
        return file;
    }

private:
    void patch(const std::string& target)
    {
        patches_.emplace_back(code_.size(), target);
        u32(0);
    }

    int case_ = 0;
    std::vector<std::uint8_t> code_;
    std::map<std::string, std::size_t> labels_;
    std::vector<std::pair<std::size_t, std::string>> patches_;
};

int main(int argc, char* argv[])
{
    Builder b;
    b.event("GetFlag", {1001, 14});
    b.ifv(14, 4, 1, "reload_return");
    b.message("CONFORMANCE START. Each prompt describes the real opcode run next.");

    b.message("MovV: set register 0 to 5."); b.movv(0, 5); b.check(0, 5, "MovV");
    b.message("AddV: add 7 to register 0."); b.binary_v(17, 0, 7); b.check(0, 12, "AddV");
    b.message("Inc: increment register 0."); b.unary(12, 0); b.check(0, 13, "Inc");
    b.message("Dec: decrement register 0."); b.unary(13, 0); b.check(0, 12, "Dec");

    b.message("MovR: copy register 0 to register 5."); b.movr(5, 0); b.check(5, 12, "MovR");
    b.movv(6, 34); b.movv(7, 78);
    b.message("Swap: exchange registers 6 and 7."); b.swap(6, 7);
    b.check(6, 78, "Swap reg 6"); b.check(7, 34, "Swap reg 7");

    b.movv(7, 20);
    b.message("SubV: subtract 6 from register 7."); b.binary_v(19, 7, 6); b.check(7, 14, "SubV");
    b.message("MulV: multiply register 7 by 3."); b.binary_v(21, 7, 3); b.check(7, 42, "MulV");
    b.message("DivV: divide register 7 by 4."); b.binary_v(23, 7, 4); b.check(7, 10, "DivV");
    b.message("ModV: modulo register 7 by 3."); b.binary_v(25, 7, 3); b.check(7, 1, "ModV");

    b.movv(8, 0b1100);
    b.message("AndV: register 8 &= 0b1010."); b.binary_v(27, 8, 0b1010); b.check(8, 0b1000, "AndV");
    b.message("OrV: register 8 |= 0b0011."); b.binary_v(29, 8, 0b0011); b.check(8, 0b1011, "OrV");
    b.message("XorV: register 8 ^= 0b1011."); b.binary_v(31, 8, 0b1011); b.check(8, 0b0000, "XorV");

    b.movv(9, 5);
    b.message("Neg: negate register 9."); b.unary(15, 9); b.check(9, -5, "Neg");
    b.movv(9, 0b101010);
    b.message("Not: bitwise not register 9."); b.unary(14, 9); b.check(9, ~0b101010, "Not");

    b.movv(10, 7); b.movv(11, 7); b.movv(12, 7);
    b.message("IfV comparisons: <, <=, >, >=, !=, ==.");
    b.compare_test(10, 0, 10, "IfV_lt");
    b.compare_test(10, 1, 10, "IfV_le");
    b.compare_test(10, 2, 5, "IfV_gt");
    b.compare_test(10, 3, 5, "IfV_ge");
    b.compare_test(10, 4, 7, "IfV_eq");
    b.compare_test(10, 5, 6, "IfV_ne");

    b.movv(13, 2); b.movv(14, 0); b.label("loop_body2"); b.unary(12, 14);
    b.loop(13, "loop_body2"); b.message("Loop: second loop ran three times."); b.check(14, 3, "Loop2");

    std::vector<std::uint8_t> expr;
    auto append = [&](const std::vector<std::uint8_t>& part) {
        expr.insert(expr.end(), part.begin(), part.end());
    };
    append(Builder::calc_imm(5));
    append(Builder::calc_imm(3));
    append(Builder::calc_op(0)); // +
    append(Builder::calc_imm(2));
    append(Builder::calc_op(2)); // *
    b.message("Calc: evaluate (5 + 3) * 2 in postfix."); b.calc(16, expr); b.check(16, 16, "Calc");

    b.message("Rand: register receives a random value."); b.rand(4);
    b.message("Run: yield exactly one rendered frame."); b.run();
    b.message("Wait: pause for 30 frames."); b.wait(30);

    b.message("Mov2: set register 5 to 123."); b.event("Mov2", {5, 123}); b.check(5, 123, "Mov2");
    b.message("Sin: sin(90 degrees) scaled to 4096."); b.event("Sin", {6, 900, 4096}); b.check(6, 4096, "Sin");
    b.message("Cos: cos(0 degrees) scaled to 4096."); b.event("Cos", {7, 0, 4096}); b.check(7, 4096, "Cos");
    b.message("Abs: absolute value of -42."); b.event("Abs", {17, -42}); b.check(17, 42, "Abs");
    b.message("SetFlag/GetFlag: temporary flag 1000 round trip.");
    b.event("SetFlag", {1000, 314}); b.event("GetFlag", {1000, 8}); b.check(8, 314, "SetFlag/GetFlag");
    b.event("SetFlag", {1000, 0});
    b.message("SetGameFlag/GetGameFlag: temporary game flag 1000 round trip.");
    b.event("SetGameFlag", {1000, 271}); b.event("GetGameFlag", {1000, 10});
    b.check(10, 271, "SetGameFlag/GetGameFlag"); b.event("SetGameFlag", {1000, 0});
    b.message("GetTime/WaitTime: capture timer, wait briefly.");
    b.event("GetTime", {11}); b.event("WaitTime", {40});
    b.message("GetSystemTime: fill hour/day/month/year registers.");
    b.event("GetSystemTime", {11, 12, 13, 14});
    b.message("WaitFrame: wait 30 rendered frames."); b.event("WaitFrame", {30});

    b.message("SetMessage2: this block was created by SetMessage2.");
    b.append("\\nAddMessage2: this line is appended with an explicit newline.");
    b.message("T: hide message for 30 frames, then show it.");
    b.event("T", {0, -1}); b.wait(30); b.event("T", {1, -1});
    b.message("K: present an explicit end-of-block input wait."); b.event("K", {});
    b.message("W: original-engine no-op; should continue normally."); b.event("W", {-1});

    b.message("B: fade to background 55."); b.event("B", {1, 55, 0, 30, -1, -1, -1});
    b.message("BT: transition to background 71."); b.event("BT", {1, 71, 0, 15, -1, -1, -1});
    b.message("BC: change background to 56."); b.event("BC", {1, 56, 0, 30, -1, -1, -1});
    b.message("BCT: transition background to 26."); b.event("BCT", {1, 26, 0, 30, -1, -1, -1});
    b.message("V: display event graphic 103 variant 2."); b.event("V", {1, 103, 2, -1, -1, -1, -1, -1});
    b.message("VT: transition event graphic 113 variant 1."); b.event("VT", {1, 113, 1, 1, -1, -1, -1, -1});
    b.message("H: display CG 100."); b.event("H", {1, 100, 0, 30, -1, -1, -1, -1});
    b.message("HT: transition CG 100."); b.event("HT", {1, 100, 44, 60, -1, -1, -1, -1});
    b.message("S: slide art up, jump below the screen, then ease back.");
    b.event("S", {0, -448, 16, 1});
    b.event("S", {0, 448, 0, 0});
    b.event("S", {0, 0, 8, 1});
    b.message("Z: zoom the art layer."); b.event("Z", {100, 0, 700, 525, 30, 0});
    b.message("FB: darken then restore brightness."); b.event("FB", {0, 0, 0, 20});
    b.event("FB", {128, 128, 128, 20});
    b.message("Q: shake the screen briefly."); b.event("Q", {1, 10, 15, 2});
    b.message("F: fade through white."); b.event("F", {255, 255, 255, 5, 5});
    b.message("SetShake: scripted shake."); b.event("SetShake", {2, 25, 8, 0, -1});

    b.message("C: show character 1, pose 3010."); b.event("C", {1, 3010, 1, -1, -1, -1, -1, -1});
    b.message("CP: change character 1 to pose 1021."); b.event("CP", {1, 1021, -1});
    b.message("CL: move character 1 to location 3."); b.event("CL", {1, 3, -1});
    b.message("CW: stage pose 3020; the following BC should reveal it.");
    b.event("CW", {1, 3020, 1, -1, -1, -1});
    b.event("BC", {1, 56, 0, 30, -1, -1, -1});
    b.message("CW/BC: character 1 should now be visible in pose 3020.");
    b.message("CR: remove character 1."); b.event("CR", {1, -1, -1});
    b.message("C: display character 10 for the CRW test."); b.event("C", {10, 80, 0, -1, -1, -1, -1, -1});
    b.message("CRW: stage character 10 for removal; it remains visible.");
    b.event("CRW", {10, -1});
    b.message("CRW: character 10 should still be visible until this BC.");
    b.event("BC", {1, 55, 0, 30, -1, -1, -1});
    b.message("CRW/BC: character 10 should now be gone.");

    b.message("SetBmpEx: show B017000.bmp above the background.");
    b.event("SetBmpEx", {8, 1, "B017000.bmp", 2, -1, 0, "bak"});
    b.message("SetBmpParam: make the overlay half transparent."); b.event("SetBmpParam", {8, 11, 128});
    b.message("SetBmpBright: darken the overlay."); b.event("SetBmpBright", {8, 64, 64, 64});
    b.message("SetBmpMove: move the overlay down and right."); b.event("SetBmpMove", {8, 80, 40});
    b.message("SetBmpZoom: shrink the overlay."); b.event("SetBmpZoom", {8, 80, 40, 480, 336});
    b.message("ResetBmp: remove overlay slot 8."); b.event("ResetBmp", {8});

    b.message("M: start BGM track 1."); b.event("M", {1, 0, -1, -1});
    b.message("MV: fade BGM volume down."); b.event("MV", {128, 30});
    b.message("MW: wait for the BGM fade."); b.event("MW", {});
    b.message("MS: stop BGM."); b.event("MS", {30}); b.event("MW", {});
    b.message("SE: play sound effect 210."); b.event("SE", {210, -1});
    b.message("SEP: play sound effect 1017 on channel 0."); b.event("SEP", {0, 1017, 0, 0, 255});
    b.message("SEV: fade channel 0 volume."); b.event("SEV", {0, 128, 15});
    b.message("SEW: wait for channel 0 playback."); b.event("SEW", {0});
    b.message("SES: stop channel 0."); b.event("SES", {0, 0});

    b.message("VI: override voice scenario to opening scene."); b.event("VI", {1, -1, 10301000});
    b.message("VV: play opening voice 1."); b.event("VV", {1, -1, -1, 1, -1});
    b.message("VW: wait for voice completion."); b.event("VW", {-1});
    b.message("VC: play conditional opening voice 15."); b.event("VC", {1, -1, -1, 15, -1});
    b.message("VS: stop voice with a short fade."); b.event("VS", {15, -1});
    b.message("VA: play character-8 voice from scenario 080305000."); b.event("VI", {1, -1, 80305000});
    b.event("VA", {8, -1, -1, 1, -1}); b.event("VW", {-1});
    b.message("VB: play alternate voice form."); b.event("VB", {1, -1, -1, 29, -1}); b.event("VS", {0, -1});

    b.message("SetSelectMes/SetSelect: choose either option; suite continues.");
    b.event("SetSelectMes", {"Choice one\\n(newline)", -1, -1, -1});
    b.event("SetSelectMes", {"Choice two", -1, -1, -1});
    b.event("SetSelect", {0});
    b.message("Choice returned through the real selection register.");

    b.message("SetTimeMode: use daytime mode."); b.event("SetTimeMode", {0, -1});
    b.message("SetWeatherMode: enable weather mode briefly."); b.event("SetWeatherMode", {1});
    b.event("SetWeatherMode", {0});
    b.message("ViewClock: animate the clock UI."); b.event("ViewClock", {11, -1, -1});
    b.message("SkipDate: set pending date jump to 4/7."); b.event("SkipDate", {4, 7});
    b.message("ViewCalender: animate the calendar UI to 4/7."); b.event("ViewCalender", {-1, -1});
    b.check_flag(0, 4, "SkipDate month applied");
    b.check_flag(1, 7, "SkipDate day applied");
    b.message("SetMapEvent: register a map destination without entering it.");
    b.event("SetMapEvent", {1, 6, 3, "010301200"});
    b.message("SetSakura: begin sakura particles."); b.event("SetSakura", {32, 1, 0});
    b.wait(90);
    b.message("StopSakura: stop particles."); b.event("StopSakura", {});
    b.message("VIB: PC original no-op; should continue."); b.event("VIB", {1, 30, 255});
    b.message("SetDemoFlag: set then clear temporary demo flag."); b.event("SetDemoFlag", {1, 0});
    b.event("SetDemoFlag", {0, 0});
    b.message("SetReplayNo: invoke shipped replay unlock 1."); b.event("SetReplayNo", {1});

    b.message("LoadScript: reload this SDT, guarded by temporary flag 1001.");
    b.event("SetFlag", {1001, 1});
    b.event("LoadScript", {"999999999"});
    b.label("reload_return");
    b.event("SetFlag", {1001, 0});
    b.message("PASS: LoadScript returned to the guarded continuation.");
    b.message("SetMovie: play leaf logo (movie 3). Continue afterward if supported."); b.event("SetMovie", {3});
    b.message("SetEnding: final destructive test; expected to leave gameplay.");
    b.event("SetEnding", {1, 0});
    b.message("SetTitle: if ending returned, go to title now."); b.event("SetTitle", {});
    b.end();

    const auto file = b.finish();
    const std::string path = argc > 1 ? argv[1] : "conformance/999999999.SDT";
    std::ofstream output(path, std::ios::binary);
    if (!output) {
        throw std::runtime_error("failed to open output " + path);
    }
    output.write(reinterpret_cast<const char*>(file.data()), file.size());
}
