#include "tiny/synth.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <utility>

namespace tiny {
namespace {

constexpr float tau = 2.0F * std::numbers::pi_v<float>;

float midi_to_frequency(float note) {
    return 440.0F * std::exp2((note - 69.0F) / 12.0F);
}

std::uint64_t seconds_to_samples(float seconds) {
    return static_cast<std::uint64_t>(
        std::max(1.0F, seconds * static_cast<float>(sample_rate)));
}

} // namespace

SynthEngine::SynthEngine(Song song) : song_(std::move(song)) {}

void SynthEngine::render(std::span<float> output) {
    const std::size_t frame_count = output.size() / 2;
    const auto step_samples = song_.samples_per_step();

    for (std::size_t frame = 0; frame < frame_count; ++frame) {
        if (loop_enabled_ && sample_position_ >= loop_end_sample_) {
            sample_position_ = loop_start_sample_;
            // De-clicked instruments keep their natural release tail across
            // the loop boundary. Raw instruments retain the intentional hard
            // cut behavior.
            for (auto& voice : voices_) {
                if (voice.active && !voice.instrument.declick) {
                    voice.active = false;
                }
            }
        }
        if (sample_position_ % step_samples == 0) {
            const auto step_index =
                (sample_position_ / step_samples) % song_step_count;
            trigger_step(static_cast<std::size_t>(step_index));
        }

        float left = 0.0F;
        float right = 0.0F;

        for (auto& voice : voices_) {
            if (!voice.active) {
                continue;
            }

            const float sample = render_voice(voice);
            const float pan = std::clamp(voice.instrument.pan, -1.0F, 1.0F);
            left += sample * (0.5F - pan * 0.5F);
            right += sample * (0.5F + pan * 0.5F);
        }

        // A small soft clipper catches voice summing peaks without a limiter.
        output[frame * 2] = left / (1.0F + std::abs(left));
        output[frame * 2 + 1] = right / (1.0F + std::abs(right));
        ++sample_position_;
    }
}

void SynthEngine::reset() {
    voices_ = {};
    sample_position_ = loop_enabled_ ? loop_start_sample_ : 0;
    next_voice_ = 0;
}

void SynthEngine::seek_to_step(std::size_t step) {
    voices_ = {};
    sample_position_ =
        static_cast<std::uint64_t>(step % song_step_count) *
        song_.samples_per_step();
    next_voice_ = 0;
}

void SynthEngine::set_loop_steps(std::size_t start_step,
                                 std::size_t length_steps) {
    loop_enabled_ = true;
    loop_start_step_ = std::min(start_step, song_step_count - 1);
    loop_length_steps_ = std::clamp<std::size_t>(
        length_steps, 1, song_step_count - loop_start_step_);
    refresh_loop_bounds();
    if (sample_position_ < loop_start_sample_ ||
        sample_position_ >= loop_end_sample_) {
        seek_to_step(loop_start_step_);
    }
}

void SynthEngine::clear_loop() {
    loop_enabled_ = false;
    loop_start_step_ = 0;
    loop_length_steps_ = 0;
    loop_start_sample_ = 0;
    loop_end_sample_ = 0;
}

void SynthEngine::set_song(Song song, bool reset_transport) {
    song_ = std::move(song);
    if (loop_enabled_) {
        refresh_loop_bounds();
    }
    if (reset_transport) {
        reset();
    }
}

void SynthEngine::audition(int midi_note, std::size_t instrument,
                           float duration_seconds) {
    if (instrument >= song_.instruments().size()) {
        return;
    }
    const auto gate_samples = static_cast<std::uint64_t>(
        std::max(0.01F, duration_seconds) * static_cast<float>(sample_rate));
    trigger(
        {
            .note = static_cast<std::int8_t>(std::clamp(midi_note, 0, 127)),
            .instrument = static_cast<std::uint8_t>(instrument),
            .gate_steps = 1,
        },
        instrument, gate_samples);
}

std::uint64_t SynthEngine::sample_position() const {
    return sample_position_;
}

const Song& SynthEngine::song() const {
    return song_;
}

void SynthEngine::trigger_step(std::size_t step_index) {
    const auto& current_step = song_.step(step_index);
    for (std::size_t track = 0; track < current_step.tracks.size(); ++track) {
        if (current_step.tracks[track].note >= 0) {
            trigger(current_step.tracks[track], track);
        }
    }
}

void SynthEngine::trigger(const StepEvent& event, std::size_t track,
                          std::uint64_t gate_samples) {
    auto& voice = voices_[next_voice_++ % voices_.size()];
    if (gate_samples == 0) {
        gate_samples =
            static_cast<std::uint64_t>(event.gate_steps) *
            song_.samples_per_step();
    }
    voice = {
        .instrument = song_.instrument(event.instrument),
        .active = true,
        .midi_note = static_cast<float>(event.note),
        .phase = 0.0F,
        .filter_low = 0.0F,
        .filter_band = 0.0F,
        .release_level = 0.0F,
        .age = 0,
        .gate_samples = gate_samples,
        .noise_state =
            0x9e3779b9U ^ static_cast<std::uint32_t>(sample_position_) ^
            static_cast<std::uint32_t>(track * 0x85ebca6bU),
    };
}

float SynthEngine::oscillator(Voice& voice) {
    switch (voice.instrument.waveform) {
    case Waveform::sine:
        return std::sin(voice.phase * tau);
    case Waveform::triangle:
        return 1.0F - 4.0F * std::abs(voice.phase - 0.5F);
    case Waveform::saw:
        return voice.phase * 2.0F - 1.0F;
    case Waveform::pulse:
        return voice.phase < voice.instrument.pulse_width ? 1.0F : -1.0F;
    case Waveform::noise:
        voice.noise_state ^= voice.noise_state << 13U;
        voice.noise_state ^= voice.noise_state >> 17U;
        voice.noise_state ^= voice.noise_state << 5U;
        return static_cast<float>(voice.noise_state & 0xffffU) / 32767.5F -
               1.0F;
    }
    return 0.0F;
}

float SynthEngine::render_voice(Voice& voice) {
    const auto release_samples =
        seconds_to_samples(voice.instrument.release_seconds);
    if (voice.age >= voice.gate_samples + release_samples) {
        voice.active = false;
        return 0.0F;
    }

    const float age_seconds =
        static_cast<float>(voice.age) / static_cast<float>(sample_rate);
    float note = voice.midi_note;
    if (voice.instrument.pitch_drop_semitones > 0.0F) {
        const float drop_time =
            std::max(0.001F, voice.instrument.pitch_drop_seconds);
        note += voice.instrument.pitch_drop_semitones *
                std::exp(-age_seconds / drop_time);
    }

    const float frequency = midi_to_frequency(note);
    const float raw = oscillator(voice);
    voice.phase += frequency / static_cast<float>(sample_rate);
    voice.phase -= std::floor(voice.phase);

    float filtered = raw;
    if (voice.instrument.filter_cutoff < 0.999F) {
        const float coefficient =
            std::clamp(voice.instrument.filter_cutoff, 0.01F, 0.99F);
        const float damping =
            1.8F - std::clamp(voice.instrument.filter_resonance, 0.0F, 0.95F);
        const float high =
            raw - voice.filter_low - damping * voice.filter_band;
        voice.filter_band += coefficient * high;
        voice.filter_low += coefficient * voice.filter_band;
        filtered = voice.filter_low;
    }

    float edge_fade = 1.0F;
    if (voice.instrument.declick) {
        constexpr float fade_seconds = 0.003F;
        const auto fade_samples = seconds_to_samples(fade_seconds);
        const auto total_samples = voice.gate_samples + release_samples;
        const auto remaining_samples =
            total_samples > voice.age ? total_samples - voice.age : 0;
        const float fade_in = std::min(
            1.0F, static_cast<float>(voice.age) /
                      static_cast<float>(fade_samples));
        const float fade_out = std::min(
            1.0F, static_cast<float>(remaining_samples) /
                      static_cast<float>(fade_samples));
        edge_fade = std::min(fade_in, fade_out);
    }

    const float result = filtered * envelope(voice) * voice.instrument.gain *
                         edge_fade;
    ++voice.age;
    return result;
}

float SynthEngine::envelope(const Voice& voice) const {
    const auto attack = seconds_to_samples(voice.instrument.attack_seconds);
    const auto decay = seconds_to_samples(voice.instrument.decay_seconds);

    if (voice.age < attack) {
        return static_cast<float>(voice.age) / static_cast<float>(attack);
    }
    if (voice.age < attack + decay) {
        const float phase =
            static_cast<float>(voice.age - attack) / static_cast<float>(decay);
        return 1.0F +
               (voice.instrument.sustain - 1.0F) * std::clamp(phase, 0.0F, 1.0F);
    }
    if (voice.age < voice.gate_samples) {
        return voice.instrument.sustain;
    }

    const auto release = seconds_to_samples(voice.instrument.release_seconds);
    const float release_phase =
        static_cast<float>(voice.age - voice.gate_samples) /
        static_cast<float>(release);
    return voice.instrument.sustain *
           (1.0F - std::clamp(release_phase, 0.0F, 1.0F));
}

void SynthEngine::refresh_loop_bounds() {
    const auto step_samples =
        static_cast<std::uint64_t>(song_.samples_per_step());
    loop_start_sample_ =
        static_cast<std::uint64_t>(loop_start_step_) * step_samples;
    loop_end_sample_ =
        static_cast<std::uint64_t>(loop_start_step_ + loop_length_steps_) *
        step_samples;
}

} // namespace tiny
