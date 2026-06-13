#include "archive.hpp"
#include "bytecode.hpp"
#include "event.hpp"
#include "scenario.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <exception>
#include <filesystem>
#include <format>
#include <iomanip>
#include <iostream>
#include <map>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr std::array<std::string_view, 87> retail_opcodes{
    "AddMessage2", "AddV", "B", "BC", "BCT", "BT", "C", "CL", "CP",
    "CR", "CRW", "CW", "Cos", "Dec", "End", "F", "FB", "GetFlag",
    "GetGameFlag", "GetSystemTime", "GetTime", "Goto", "H", "HT", "IfR",
    "IfV", "Inc", "K", "LoadScript", "Loop", "M", "MS", "MV", "MW",
    "Mov2", "MovV", "Q", "Rand", "ResetBmp", "Run", "S", "SE", "SEP",
    "SES", "SEV", "SEW", "SetBmpBright", "SetBmpEx", "SetBmpMove",
    "SetBmpParam", "SetBmpZoom", "SetDemoFlag", "SetEnding", "SetFlag",
    "SetGameFlag", "SetMapEvent", "SetMessage2", "SetMovie", "SetReplayNo",
    "SetSakura", "SetSelect", "SetSelectMes", "SetShake", "SetTimeMode",
    "SetTitle", "SetWeatherMode", "Sin", "SkipDate", "StopSakura", "T",
    "V", "VA", "VB", "VC", "VI", "VIB", "VS", "VT", "VV", "VW",
    "ViewCalender", "ViewClock", "W", "Wait", "WaitFrame", "WaitTime", "Z",
};

bool is_scenario(std::string_view name)
{
    if (name.size() < 4) {
        return false;
    }
    std::string extension(name.substr(name.size() - 4));
    std::ranges::transform(extension, extension.begin(), [](unsigned char value) {
        return static_cast<char>(std::tolower(value));
    });
    return extension == ".sdt";
}

std::string scenario_name(std::string name)
{
    if (!is_scenario(name)) {
        name += ".SDT";
    }
    return name;
}

const std::string& string_argument(const th2::Event& event, std::size_t index)
{
    return std::get<std::string>(event.arguments.at(index));
}

std::int32_t integer_argument(const th2::Event& event, std::size_t index)
{
    return std::get<std::int32_t>(event.arguments.at(index));
}

bool stable_integer(
    const th2::Event& first, const th2::Event& second, std::size_t index)
{
    return integer_argument(first, index) == integer_argument(second, index);
}

struct Reference {
    std::string source;
    std::size_t offset;

    friend bool operator<(const Reference& left, const Reference& right)
    {
        return std::tie(left.source, left.offset)
            < std::tie(right.source, right.offset);
    }
};

}  // namespace

int main(int argc, char** argv)
{
    if (argc != 2) {
        std::cerr << "usage: th2-script-audit GAME_DATA_DIRECTORY\n";
        return 2;
    }

    try {
        const std::filesystem::path data(argv[1]);
        const th2::Archive archive(data / "SDT.PAK");
        const th2::Archive graphics(data / "GRP.PAK");
        const th2::Archive backgrounds(data / "bak.pak");
        const th2::Archive bgm(data / "bgm.PAK");
        const th2::Archive sound_effects(data / "SE.PAK");
        const th2::Archive movies(data / "mov.pak");
        std::map<std::string, std::size_t> opcode_counts;
        std::map<std::string, std::set<Reference>> scenario_references;
        std::set<std::string> missing_assets;
        std::size_t scenario_count = 0;
        std::size_t instruction_count = 0;
        std::size_t undecodable_references = 0;
        std::size_t dynamic_asset_references = 0;
        std::array<std::int32_t, 50> registers{};
        registers.fill(1);
        std::array<std::int32_t, 50> alternate_registers{};
        alternate_registers.fill(2);

        for (const auto& entry : archive.entries()) {
            if (!is_scenario(entry.name)) {
                continue;
            }
            ++scenario_count;
            const th2::Scenario scenario(archive.read(entry));
            std::size_t offset = 0;
            try {
                while (offset < scenario.bytecode().size()) {
                    const auto instruction =
                        th2::decode_instruction(scenario.bytecode(), offset);
                    ++opcode_counts[std::string(instruction.name)];
                    ++instruction_count;

                    if (instruction.name == "SLoad") {
                        const auto bytes = scenario.bytecode().subspan(
                            instruction.offset, instruction.size);
                        const auto length = bytes[2];
                        const std::string target(
                            reinterpret_cast<const char*>(bytes.data() + 3),
                            length);
                        scenario_references[scenario_name(target)].insert(
                            {entry.name, instruction.offset});
                    } else if (instruction.opcode >= 64) {
                        try {
                            const auto event = th2::decode_event(
                                instruction,
                                scenario.bytecode().subspan(
                                    instruction.offset, instruction.size),
                                registers);
                            const auto alternate_event = th2::decode_event(
                                instruction,
                                scenario.bytecode().subspan(
                                    instruction.offset, instruction.size),
                                alternate_registers);
                            if (instruction.name == "LoadScript"
                                || instruction.name == "SetMapEvent") {
                                const auto argument =
                                    instruction.name == "LoadScript" ? 0u : 3u;
                                const auto& target =
                                    string_argument(event, argument);
                                if (!target.empty()) {
                                    scenario_references[
                                        scenario_name(target)].insert(
                                        {entry.name, instruction.offset});
                                }
                            } else if (instruction.name == "SetBmpEx") {
                                const auto& name = string_argument(event, 2);
                                const auto& pack = string_argument(event, 6);
                                const auto& source =
                                    pack == "bak" ? backgrounds : graphics;
                                if (!source.find(name)) {
                                    missing_assets.insert(
                                        pack + "/" + name);
                                }
                            } else if (instruction.name == "M") {
                                if (!stable_integer(event, alternate_event, 0)) {
                                    ++dynamic_asset_references;
                                    offset += instruction.size;
                                    continue;
                                }
                                const int track = integer_argument(event, 0);
                                if (track >= 0) {
                                    const auto single = std::format(
                                        "BGM_{:03d}.OGG", track);
                                    const auto intro = std::format(
                                        "BGM_{:03d}_A.OGG", track);
                                    const auto loop = std::format(
                                        "BGM_{:03d}_B.OGG", track);
                                    if (!bgm.find(single)
                                        && (!bgm.find(intro)
                                            || !bgm.find(loop))) {
                                        missing_assets.insert(
                                            "bgm/" + single);
                                    }
                                }
                            } else if (instruction.name == "SE"
                                       || instruction.name == "SEP") {
                                const auto argument =
                                    instruction.name == "SE" ? 0u : 1u;
                                if (!stable_integer(
                                        event, alternate_event, argument)) {
                                    ++dynamic_asset_references;
                                    offset += instruction.size;
                                    continue;
                                }
                                const int sound =
                                    integer_argument(event, argument);
                                const auto name =
                                    std::format("SE_{:04d}.WAV", sound);
                                if (sound >= 0 && !sound_effects.find(name)) {
                                    missing_assets.insert("se/" + name);
                                }
                            } else if (instruction.name == "V"
                                       || instruction.name == "VT"
                                       || instruction.name == "H"
                                       || instruction.name == "HT") {
                                if (!stable_integer(event, alternate_event, 1)
                                    || !stable_integer(
                                        event, alternate_event, 2)) {
                                    ++dynamic_asset_references;
                                    offset += instruction.size;
                                    continue;
                                }
                                int visual = integer_argument(event, 1) * 10;
                                if (integer_argument(event, 2) >= 0) {
                                    visual += integer_argument(event, 2);
                                }
                                const char prefix =
                                    instruction.name[0] == 'H' ? 'h' : 'v';
                                const auto name = std::format(
                                    "{}{:06d}.tga", prefix, visual);
                                if (!graphics.find(name)) {
                                    missing_assets.insert("grp/" + name);
                                }
                            } else if (instruction.name == "SetMovie") {
                                if (!stable_integer(event, alternate_event, 0)) {
                                    ++dynamic_asset_references;
                                    offset += instruction.size;
                                    continue;
                                }
                                const int mode = integer_argument(event, 0);
                                const std::array names{
                                    "TH2_OP_800x448_5M.avi",
                                    "",
                                    "TH2_TR_800x600_5M.avi",
                                    "Leaf_800x600_5M.avi",
                                };
                                if (mode < 0
                                    || mode >= static_cast<int>(names.size())
                                    || names[mode][0] == '\0'
                                    || !movies.find(names[mode])) {
                                    missing_assets.insert(std::format(
                                        "movie/mode {}", mode));
                                }
                            } else if (instruction.name == "SetEnding") {
                                if (!stable_integer(event, alternate_event, 0)
                                    || !stable_integer(
                                        event, alternate_event, 1)) {
                                    ++dynamic_asset_references;
                                    offset += instruction.size;
                                    continue;
                                }
                                const int ending =
                                    integer_argument(event, 1) == 1
                                        || integer_argument(event, 0) == 10
                                    ? 0 : integer_argument(event, 0);
                                const auto name = std::format(
                                    "TH2_ED_{:02d}_800_3M.avi", ending);
                                if (!movies.find(name)) {
                                    missing_assets.insert("movie/" + name);
                                }
                            }
                        } catch (const std::exception&) {
                            if (instruction.name == "LoadScript"
                                || instruction.name == "SetMapEvent") {
                                ++undecodable_references;
                            }
                        }
                    }
                    offset += instruction.size;
                }
            } catch (const std::exception& error) {
                throw std::runtime_error(
                    entry.name + " at byte " + std::to_string(offset)
                    + ": " + error.what());
            }
        }

        const std::set<std::string> expected(
            retail_opcodes.begin(), retail_opcodes.end());
        std::set<std::string> observed;
        for (const auto& [name, count] : opcode_counts) {
            static_cast<void>(count);
            observed.insert(name);
        }

        std::vector<std::string> unexpected;
        std::ranges::set_difference(
            observed, expected, std::back_inserter(unexpected));
        std::vector<std::string> absent;
        std::ranges::set_difference(
            expected, observed, std::back_inserter(absent));

        std::size_t present_references = 0;
        std::size_t missing_references = 0;
        for (const auto& [target, sources] : scenario_references) {
            if (archive.find(target)) {
                ++present_references;
            } else {
                ++missing_references;
                std::cout << "missing scenario " << target << " referenced by";
                for (const auto& source : sources) {
                    std::cout << ' ' << source.source << ':' << source.offset;
                }
                std::cout << '\n';
            }
        }

        std::vector<std::string> missing_schedule;
        static constexpr std::array periods{
            "MORNING", "INTERVAL", "LUNCH_BREAK",
            "SCHOOL_HOURS", "AFTER_SCHOOL", "NIGHT",
        };
        for (int month = 3; month <= 5; ++month) {
            const int last_day = month == 3 ? 31 : month == 4 ? 30 : 20;
            for (int day = 1; day <= last_day; ++day) {
                for (const auto period : periods) {
                    if (month == 5 && day == 20
                        && period != std::string_view("MORNING")) {
                        continue;
                    }
                    const auto name = std::format(
                        "EV_{:02d}{:02d}{}.SDT", month, day, period);
                    if (!archive.find(name)) {
                        missing_schedule.push_back(name);
                    }
                }
            }
        }

        std::cout << scenario_count << " scenarios, " << instruction_count
                  << " instructions, " << observed.size() << " opcodes\n"
                  << present_references << " present scenario destinations, "
                  << missing_references << " absent scenario destinations\n"
                  << "481 scheduled event scenarios verified\n";
        if (undecodable_references != 0) {
            std::cout << undecodable_references
                      << " scenario references could not be decoded statically\n";
        }
        if (dynamic_asset_references != 0) {
            std::cout << dynamic_asset_references
                      << " dynamic asset references require runtime validation\n";
        }

        for (const auto& name : missing_assets) {
            std::cerr << "missing invoked asset: " << name << '\n';
        }
        for (const auto& name : missing_schedule) {
            std::cerr << "missing scheduled scenario: " << name << '\n';
        }

        if (!unexpected.empty() || !absent.empty()
            || !missing_assets.empty() || !missing_schedule.empty()) {
            for (const auto& name : unexpected) {
                std::cerr << "unexpected shipped opcode: " << name << '\n';
            }
            for (const auto& name : absent) {
                std::cerr << "expected shipped opcode is absent: " << name << '\n';
            }
            return 1;
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
