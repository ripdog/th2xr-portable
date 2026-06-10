#include "archive.hpp"

#include <algorithm>
#include <exception>
#include <iostream>

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::cerr << "usage: th2-archive-info ARCHIVE...\n";
        return 2;
    }

    try {
        for (int arg = 1; arg < argc; ++arg) {
            const th2::Archive archive(argv[arg]);
            const auto compressed = std::count_if(
                archive.entries().begin(), archive.entries().end(),
                [](const th2::ArchiveEntry& entry) { return entry.compressed; });
            std::cout << archive.path() << ": "
                      << (archive.kind() == th2::ArchiveKind::kcap ? "KCAP" : "LAC")
                      << ", " << archive.entries().size() << " entries, "
                      << compressed << " compressed\n";

            const auto shown = std::min<std::size_t>(archive.entries().size(), 8);
            for (std::size_t i = 0; i < shown; ++i) {
                const auto& entry = archive.entries()[i];
                std::cout << "  " << entry.name << "  " << entry.stored_size
                          << " bytes" << (entry.compressed ? " (compressed)" : "") << '\n';
            }
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

