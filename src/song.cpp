#include "tiny/song.hpp"

#include <algorithm>
#include <cmath>

namespace tiny {
namespace {

StepEvent note(int midi_note, std::size_t instrument, int gate_steps = 1) {
    return {
        .note = static_cast<std::int8_t>(midi_note),
        .instrument = static_cast<std::uint8_t>(instrument),
        .gate_steps = static_cast<std::uint8_t>(gate_steps),
    };
}

} // namespace

Song Song::make_demo_song() {
    Song song;

    // Kick
    song.instruments_[0] = {
        .waveform = Waveform::sine,
        .attack_seconds = 0.001F,
        .decay_seconds = 0.08F,
        .sustain = 0.0F,
        .release_seconds = 0.14F,
        .pulse_width = 0.5F,
        .filter_cutoff = 1.0F,
        .filter_resonance = 0.0F,
        .pitch_drop_semitones = 34.0F,
        .pitch_drop_seconds = 0.055F,
        .gain = 0.85F,
        .pan = 0.0F,
    };

    // Bass
    song.instruments_[1] = {
        .waveform = Waveform::saw,
        .attack_seconds = 0.004F,
        .decay_seconds = 0.12F,
        .sustain = 0.55F,
        .release_seconds = 0.09F,
        .pulse_width = 0.5F,
        .filter_cutoff = 0.16F,
        .filter_resonance = 0.18F,
        .pitch_drop_semitones = 0.0F,
        .pitch_drop_seconds = 0.1F,
        .gain = 0.34F,
        .pan = -0.12F,
    };

    // Lead
    song.instruments_[2] = {
        .waveform = Waveform::pulse,
        .attack_seconds = 0.012F,
        .decay_seconds = 0.16F,
        .sustain = 0.38F,
        .release_seconds = 0.22F,
        .pulse_width = 0.28F,
        .filter_cutoff = 0.32F,
        .filter_resonance = 0.12F,
        .pitch_drop_semitones = 0.0F,
        .pitch_drop_seconds = 0.1F,
        .gain = 0.18F,
        .pan = 0.28F,
    };

    // Hat
    song.instruments_[3] = {
        .waveform = Waveform::noise,
        .attack_seconds = 0.001F,
        .decay_seconds = 0.025F,
        .sustain = 0.0F,
        .release_seconds = 0.035F,
        .pulse_width = 0.5F,
        .filter_cutoff = 0.75F,
        .filter_resonance = 0.05F,
        .pitch_drop_semitones = 0.0F,
        .pitch_drop_seconds = 0.1F,
        .gain = 0.12F,
        .pan = 0.2F,
    };

    constexpr std::array bass_notes{36, 36, 43, 34, 36, 39, 43, 34};
    constexpr std::array lead_notes{60, 63, 67, 70, 67, 63, 62, 58};

    for (std::size_t step_index = 0; step_index < song.steps_.size(); ++step_index) {
        const auto within_bar = step_index % 16;
        auto& step = song.steps_[step_index];

        if (within_bar == 0 || within_bar == 8 || within_bar == 10) {
            step.tracks[0] = note(36, 0);
        }
        if (within_bar % 2 == 0) {
            step.tracks[3] = note(84, 3);
        }
        if (within_bar % 4 == 2) {
            step.tracks[3] = note(84, 3);
        }

        if (within_bar % 2 == 0) {
            const auto bass_index = (step_index / 2) % bass_notes.size();
            step.tracks[1] = note(bass_notes[bass_index], 1, 2);
        }

        // Bring the lead in after the first bar and leave breathing room.
        if (step_index >= 16 && within_bar % 2 == 0 && within_bar != 6) {
            const auto lead_index = (step_index / 2) % lead_notes.size();
            const int octave = step_index >= 48 ? 12 : 0;
            step.tracks[2] = note(lead_notes[lead_index] + octave, 2, 2);
        }
    }

    return song;
}

std::span<const Instrument> Song::instruments() const {
    return instruments_;
}

const Instrument& Song::instrument(std::size_t index) const {
    return instruments_.at(index);
}

Instrument& Song::instrument(std::size_t index) {
    return instruments_.at(index);
}

const Step& Song::step(std::size_t index) const {
    return steps_.at(index % steps_.size());
}

Step& Song::step(std::size_t index) {
    return steps_.at(index % steps_.size());
}

std::uint32_t Song::bpm() const {
    return bpm_;
}

void Song::set_bpm(std::uint32_t bpm) {
    bpm_ = std::clamp(bpm, 40U, 300U);
}

std::uint32_t Song::samples_per_step() const {
    return (sample_rate * 60U) / (bpm_ * steps_per_beat);
}

StepRange Song::populated_range() const {
    std::size_t first = song_step_count;
    std::size_t last_exclusive = 0;
    for (std::size_t step_index = 0; step_index < steps_.size();
         ++step_index) {
        const bool populated = std::ranges::any_of(
            steps_[step_index].tracks,
            [](const StepEvent& event) { return event.note >= 0; });
        if (populated) {
            first = std::min(first, step_index);
            last_exclusive = std::max(last_exclusive, step_index + 1);
        }
    }
    if (first == song_step_count) {
        return {};
    }

    const std::size_t start = first / steps_per_bar * steps_per_bar;
    const std::size_t end = std::min<std::size_t>(
        song_step_count,
        ((last_exclusive + steps_per_bar - 1) / steps_per_bar) *
            steps_per_bar);
    return {
        .start = start,
        .length = std::max<std::size_t>(steps_per_bar, end - start),
    };
}

SyncState Song::sync_at(std::uint64_t sample_position) const {
    const double seconds = static_cast<double>(sample_position) / sample_rate;
    const double beat = seconds * static_cast<double>(bpm_) / 60.0;
    const double beat_floor = std::floor(beat);
    const float beat_phase = static_cast<float>(beat - beat_floor);
    const double bar_position = beat / beats_per_bar;
    const float bar_phase =
        static_cast<float>(bar_position - std::floor(bar_position));
    const auto step_samples = samples_per_step();
    const auto step = static_cast<std::uint32_t>(
        (sample_position / step_samples) % song_step_count);
    const auto bar = static_cast<std::uint32_t>(
        (sample_position / step_samples) /
        (steps_per_beat * beats_per_bar));

    return {
        .seconds = seconds,
        .beat = beat,
        .beat_phase = beat_phase,
        .bar_phase = bar_phase,
        .pulse = std::exp(-beat_phase * 8.0F),
        .step = step,
        .bar = bar,
    };
}

} // namespace tiny
