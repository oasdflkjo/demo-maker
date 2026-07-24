#pragma once

#include "tiny/song.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace tiny {

inline constexpr std::size_t max_pattern_steps = 64;
inline constexpr std::size_t playlist_lane_count = 8;

struct MidiPattern {
    std::string name{"Pattern"};
    std::uint8_t instrument{0};
    std::uint16_t length_steps{16};
    std::array<StepEvent, max_pattern_steps> notes{};
};

struct PlaylistClip {
    std::uint16_t start_step{0};
    std::uint8_t lane{0};
    std::uint8_t pattern{0};
};

class StudioProject {
public:
    static StudioProject make_demo();
    static StudioProject from_song(const Song& song);

    [[nodiscard]] std::uint32_t bpm() const;
    void set_bpm(std::uint32_t bpm);

    [[nodiscard]] const Instrument& instrument(std::size_t index) const;
    [[nodiscard]] Instrument& instrument(std::size_t index);
    [[nodiscard]] std::span<const Instrument> instruments() const;

    [[nodiscard]] std::span<const MidiPattern> patterns() const;
    [[nodiscard]] std::span<MidiPattern> patterns();
    [[nodiscard]] std::span<const PlaylistClip> clips() const;
    [[nodiscard]] std::span<PlaylistClip> clips();

    std::size_t add_pattern(MidiPattern pattern);
    std::size_t add_clip(PlaylistClip clip);
    void remove_clip(std::size_t index);

    [[nodiscard]] bool lane_muted(std::size_t lane) const;
    void set_lane_muted(std::size_t lane, bool muted);

    [[nodiscard]] StepRange populated_range() const;
    [[nodiscard]] Song compile_song() const;
    [[nodiscard]] Song compile_pattern(std::size_t pattern_index) const;

private:
    std::uint32_t bpm_{default_bpm};
    std::array<Instrument, track_count> instruments_{};
    std::vector<MidiPattern> patterns_;
    std::vector<PlaylistClip> clips_;
    std::array<bool, playlist_lane_count> lane_muted_{};
};

bool save_studio_project(const StudioProject& project,
                         const std::filesystem::path& path,
                         std::string& error);
bool load_studio_project(const std::filesystem::path& path,
                         StudioProject& project, std::string& error);

} // namespace tiny
