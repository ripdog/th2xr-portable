#include "archive.hpp"
#include "audio.hpp"

#include <exception>
#include <iostream>

int main(int argc, char** argv)
{
    if (argc == 1) {
        return 0;
    }
    try {
        const th2::Archive archive(argv[1]);
        const auto* entry = archive.find(argv[2]);
        if (!entry) {
            return 2;
        }
        const auto clip = th2::decode_audio(archive.read(*entry));
        if (clip.sample_rate <= 0 || clip.channels <= 0 || clip.samples.empty()) {
            return 3;
        }
        std::cout << clip.sample_rate << " Hz, " << clip.channels << " channels, "
                  << clip.samples.size() / clip.channels << " frames\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
