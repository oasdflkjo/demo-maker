#pragma once

#include "tiny/song.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace tiny {

class SynthEngine {
public:
    explicit SynthEngine(Song song = Song::make_demo_song());

    // Writes interleaved stereo floating-point samples.
    void render(std::span<float> output);
    void reset();
    void seek_to_step(std::size_t step);
    void set_loop_steps(std::size_t start_step, std::size_t length_steps);
    void clear_loop();
    void set_song(Song song, bool reset_transport = false);
    void audition(int midi_note, std::size_t instrument,
                  float duration_seconds = 0.25F);

    [[nodiscard]] std::uint64_t sample_position() const;
    [[nodiscard]] const Song& song() const;

private:
    struct Voice {
        Instrument instrument{};
        bool active{false};
        float midi_note{60.0F};
        float phase{0.0F};
        float filter_low{0.0F};
        float filter_band{0.0F};
        float release_level{0.0F};
        std::uint64_t age{0};
        std::uint64_t gate_samples{0};
        std::uint32_t noise_state{0x12345678U};
    };

    void trigger_step(std::size_t step_index);
    void trigger(const StepEvent& event, std::size_t track,
                 std::uint64_t gate_samples = 0);
    float oscillator(Voice& voice);
    float render_voice(Voice& voice);
    float envelope(const Voice& voice) const;
    void refresh_loop_bounds();

    Song song_;
    std::array<Voice, 16> voices_{};
    std::uint64_t sample_position_{0};
    std::size_t next_voice_{0};
    bool loop_enabled_{false};
    std::size_t loop_start_step_{0};
    std::size_t loop_length_steps_{0};
    std::uint64_t loop_start_sample_{0};
    std::uint64_t loop_end_sample_{0};
};

} // namespace tiny
