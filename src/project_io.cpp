#include "tiny/project_io.hpp"

#include <array>
#include <bit>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <limits>
#include <string>
#include <type_traits>

namespace tiny {
namespace {

constexpr std::uint32_t text_project_version = 1;
constexpr std::uint32_t binary_song_version = 2;

bool valid_waveform(int waveform) {
    return waveform >= static_cast<int>(Waveform::sine) &&
           waveform <= static_cast<int>(Waveform::noise);
}

template <typename T>
void write_binary_value(std::ofstream& output, T value) {
    static_assert(std::is_trivially_copyable_v<T>);
    if constexpr (std::endian::native == std::endian::big &&
                  std::is_integral_v<T>) {
        value = std::byteswap(value);
    }
    output.write(reinterpret_cast<const char*>(&value), sizeof(value));
}

template <typename T>
bool read_binary_value(std::ifstream& input, T& value) {
    static_assert(std::is_trivially_copyable_v<T>);
    input.read(reinterpret_cast<char*>(&value), sizeof(value));
    if (!input) {
        return false;
    }
    if constexpr (std::endian::native == std::endian::big &&
                  std::is_integral_v<T>) {
        value = std::byteswap(value);
    }
    return true;
}

void write_instrument_binary(std::ofstream& output,
                             const Instrument& instrument) {
    write_binary_value(output,
                       static_cast<std::uint8_t>(instrument.waveform));
    write_binary_value(output, instrument.attack_seconds);
    write_binary_value(output, instrument.decay_seconds);
    write_binary_value(output, instrument.sustain);
    write_binary_value(output, instrument.release_seconds);
    write_binary_value(output, instrument.pulse_width);
    write_binary_value(output, instrument.filter_cutoff);
    write_binary_value(output, instrument.filter_resonance);
    write_binary_value(output, instrument.pitch_drop_semitones);
    write_binary_value(output, instrument.pitch_drop_seconds);
    write_binary_value(output, instrument.gain);
    write_binary_value(output, instrument.pan);
    write_binary_value(
        output, static_cast<std::uint8_t>(instrument.declick ? 1 : 0));
}

bool read_instrument_binary(std::ifstream& input, Instrument& instrument,
                            bool has_declick) {
    std::uint8_t waveform = 0;
    if (!read_binary_value(input, waveform) ||
        !read_binary_value(input, instrument.attack_seconds) ||
        !read_binary_value(input, instrument.decay_seconds) ||
        !read_binary_value(input, instrument.sustain) ||
        !read_binary_value(input, instrument.release_seconds) ||
        !read_binary_value(input, instrument.pulse_width) ||
        !read_binary_value(input, instrument.filter_cutoff) ||
        !read_binary_value(input, instrument.filter_resonance) ||
        !read_binary_value(input, instrument.pitch_drop_semitones) ||
        !read_binary_value(input, instrument.pitch_drop_seconds) ||
        !read_binary_value(input, instrument.gain) ||
        !read_binary_value(input, instrument.pan) ||
        !valid_waveform(waveform)) {
        return false;
    }
    instrument.waveform = static_cast<Waveform>(waveform);
    if (has_declick) {
        std::uint8_t declick = 0;
        if (!read_binary_value(input, declick) || declick > 1) {
            return false;
        }
        instrument.declick = declick != 0;
    }
    return true;
}

} // namespace

bool save_song_project(const Song& song, const std::filesystem::path& path,
                       std::string& error) {
    std::ofstream output(path);
    if (!output) {
        error = "Could not open project for writing: " + path.string();
        return false;
    }

    output << "TINY_DEMO_SONG " << text_project_version << '\n';
    output << std::setprecision(9);
    output << "bpm " << song.bpm() << '\n';

    for (std::size_t index = 0; index < song.instruments().size(); ++index) {
        const auto& instrument = song.instrument(index);
        output << "instrument " << index << ' '
               << static_cast<int>(instrument.waveform) << ' '
               << instrument.attack_seconds << ' ' << instrument.decay_seconds
               << ' ' << instrument.sustain << ' '
               << instrument.release_seconds << ' ' << instrument.pulse_width
               << ' ' << instrument.filter_cutoff << ' '
               << instrument.filter_resonance << ' '
               << instrument.pitch_drop_semitones << ' '
               << instrument.pitch_drop_seconds << ' ' << instrument.gain
               << ' ' << instrument.pan << '\n';
        output << "declick " << index << ' '
               << (instrument.declick ? 1 : 0) << '\n';
    }

    for (std::size_t step = 0; step < song_step_count; ++step) {
        for (std::size_t track = 0; track < track_count; ++track) {
            const auto& event = song.step(step).tracks[track];
            if (event.note < 0) {
                continue;
            }
            output << "event " << step << ' ' << track << ' '
                   << static_cast<int>(event.note) << ' '
                   << static_cast<int>(event.instrument) << ' '
                   << static_cast<int>(event.gate_steps) << '\n';
        }
    }
    output << "end\n";

    if (!output) {
        error = "Failed while writing project: " + path.string();
        return false;
    }
    error.clear();
    return true;
}

bool load_song_project(const std::filesystem::path& path, Song& song,
                       std::string& error) {
    std::ifstream input(path);
    if (!input) {
        error = "Could not open project: " + path.string();
        return false;
    }

    std::string magic;
    std::uint32_t version = 0;
    if (!(input >> magic >> version) || magic != "TINY_DEMO_SONG" ||
        version != text_project_version) {
        error = "Unsupported project header in: " + path.string();
        return false;
    }

    Song loaded;
    std::string command;
    bool reached_end = false;
    while (input >> command) {
        if (command == "end") {
            reached_end = true;
            break;
        }
        if (command == "bpm") {
            std::uint32_t value = 0;
            if (!(input >> value)) {
                error = "Invalid BPM in project";
                return false;
            }
            loaded.set_bpm(value);
            continue;
        }
        if (command == "instrument") {
            std::size_t index = 0;
            int waveform = 0;
            Instrument instrument;
            if (!(input >> index >> waveform >> instrument.attack_seconds >>
                  instrument.decay_seconds >> instrument.sustain >>
                  instrument.release_seconds >> instrument.pulse_width >>
                  instrument.filter_cutoff >> instrument.filter_resonance >>
                  instrument.pitch_drop_semitones >>
                  instrument.pitch_drop_seconds >> instrument.gain >>
                  instrument.pan) ||
                index >= loaded.instruments().size() ||
                !valid_waveform(waveform)) {
                error = "Invalid instrument in project";
                return false;
            }
            instrument.waveform = static_cast<Waveform>(waveform);
            loaded.instrument(index) = instrument;
            continue;
        }
        if (command == "declick") {
            std::size_t index = 0;
            int enabled = 0;
            if (!(input >> index >> enabled) ||
                index >= loaded.instruments().size() ||
                (enabled != 0 && enabled != 1)) {
                error = "Invalid de-click setting in project";
                return false;
            }
            loaded.instrument(index).declick = enabled != 0;
            continue;
        }
        if (command == "event") {
            std::size_t step = 0;
            std::size_t track = 0;
            int note = 0;
            int instrument = 0;
            int gate = 0;
            if (!(input >> step >> track >> note >> instrument >> gate) ||
                step >= song_step_count || track >= track_count || note < 0 ||
                note > 127 || instrument < 0 ||
                instrument >=
                    static_cast<int>(loaded.instruments().size()) ||
                gate < 1 || gate > std::numeric_limits<std::uint8_t>::max()) {
                error = "Invalid event in project";
                return false;
            }
            loaded.step(step).tracks[track] = {
                .note = static_cast<std::int8_t>(note),
                .instrument = static_cast<std::uint8_t>(instrument),
                .gate_steps = static_cast<std::uint8_t>(gate),
            };
            continue;
        }

        error = "Unknown project command: " + command;
        return false;
    }

    if (!reached_end) {
        error = "Project is missing its end marker";
        return false;
    }

    song = loaded;
    error.clear();
    return true;
}

bool export_song_binary(const Song& song, const std::filesystem::path& path,
                        std::string& error) {
    std::ofstream output(path, std::ios::binary);
    if (!output) {
        error = "Could not open export for writing: " + path.string();
        return false;
    }

    output.write("TDSB", 4);
    write_binary_value(output, binary_song_version);
    write_binary_value(output, song.bpm());
    write_binary_value(output,
                       static_cast<std::uint32_t>(song.instruments().size()));
    write_binary_value(output, static_cast<std::uint32_t>(song_step_count));

    for (const auto& instrument : song.instruments()) {
        write_instrument_binary(output, instrument);
    }
    for (std::size_t step = 0; step < song_step_count; ++step) {
        for (const auto& event : song.step(step).tracks) {
            write_binary_value(output, event.note);
            write_binary_value(output, event.instrument);
            write_binary_value(output, event.gate_steps);
        }
    }

    if (!output) {
        error = "Failed while exporting song: " + path.string();
        return false;
    }
    error.clear();
    return true;
}

bool load_song_binary(const std::filesystem::path& path, Song& song,
                      std::string& error) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        error = "Could not open song binary: " + path.string();
        return false;
    }

    std::array<char, 4> magic{};
    std::uint32_t version = 0;
    std::uint32_t loaded_bpm = 0;
    std::uint32_t instrument_count = 0;
    std::uint32_t step_count = 0;
    input.read(magic.data(), magic.size());
    if (!input || magic != std::array{'T', 'D', 'S', 'B'} ||
        !read_binary_value(input, version) ||
        !read_binary_value(input, loaded_bpm) ||
        !read_binary_value(input, instrument_count) ||
        !read_binary_value(input, step_count) ||
        (version != 1 && version != binary_song_version) ||
        instrument_count != 4 ||
        step_count != song_step_count) {
        error = "Unsupported or damaged song binary: " + path.string();
        return false;
    }

    Song loaded;
    loaded.set_bpm(loaded_bpm);
    for (std::size_t index = 0; index < loaded.instruments().size(); ++index) {
        if (!read_instrument_binary(input, loaded.instrument(index),
                                    version >= 2)) {
            error = "Damaged instrument data in: " + path.string();
            return false;
        }
    }
    for (std::size_t step = 0; step < song_step_count; ++step) {
        for (auto& event : loaded.step(step).tracks) {
            if (!read_binary_value(input, event.note) ||
                !read_binary_value(input, event.instrument) ||
                !read_binary_value(input, event.gate_steps) ||
                event.instrument >= loaded.instruments().size() ||
                event.gate_steps == 0) {
                error = "Damaged event data in: " + path.string();
                return false;
            }
        }
    }

    song = loaded;
    error.clear();
    return true;
}

} // namespace tiny
