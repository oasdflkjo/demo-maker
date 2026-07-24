#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace tiny {

inline constexpr std::uint32_t sample_rate = 48'000;
inline constexpr std::uint32_t default_bpm = 120;
inline constexpr std::uint32_t steps_per_beat = 4;
inline constexpr std::uint32_t beats_per_bar = 4;
inline constexpr std::uint32_t steps_per_bar =
    steps_per_beat * beats_per_bar;
inline constexpr std::size_t track_count = 4;
inline constexpr std::size_t song_step_count = 256;

enum class Waveform : std::uint8_t {
    sine,
    triangle,
    saw,
    pulse,
    noise,
};

struct Instrument {
    Waveform waveform{Waveform::sine};
    float attack_seconds{0.005F};
    float decay_seconds{0.1F};
    float sustain{0.7F};
    float release_seconds{0.15F};
    float pulse_width{0.5F};
    float filter_cutoff{1.0F};
    float filter_resonance{0.0F};
    float pitch_drop_semitones{0.0F};
    float pitch_drop_seconds{0.1F};
    float gain{0.25F};
    float pan{0.0F};
    bool declick{true};
};

struct StepEvent {
    // MIDI note number. A negative value means that this track is silent.
    std::int8_t note{-1};
    std::uint8_t instrument{0};
    std::uint8_t gate_steps{1};
};

struct Step {
    std::array<StepEvent, track_count> tracks{};
};

struct SyncState {
    double seconds{0.0};
    double beat{0.0};
    float beat_phase{0.0F};
    float bar_phase{0.0F};
    float pulse{0.0F};
    std::uint32_t step{0};
    std::uint32_t bar{0};
};

struct StepRange {
    std::size_t start{0};
    std::size_t length{steps_per_bar};
};

class Song {
public:
    static Song make_demo_song();

    [[nodiscard]] std::span<const Instrument> instruments() const;
    [[nodiscard]] const Instrument& instrument(std::size_t index) const;
    [[nodiscard]] Instrument& instrument(std::size_t index);
    [[nodiscard]] const Step& step(std::size_t index) const;
    [[nodiscard]] Step& step(std::size_t index);
    [[nodiscard]] std::uint32_t bpm() const;
    void set_bpm(std::uint32_t bpm);
    [[nodiscard]] std::uint32_t samples_per_step() const;
    [[nodiscard]] StepRange populated_range() const;
    [[nodiscard]] SyncState sync_at(std::uint64_t sample_position) const;

private:
    std::uint32_t bpm_{default_bpm};
    std::array<Instrument, 4> instruments_{};
    std::array<Step, song_step_count> steps_{};
};

} // namespace tiny
