#include "tiny/studio_project.hpp"

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <limits>
#include <string>

namespace tiny {
namespace {

constexpr std::uint32_t studio_version = 1;
constexpr std::array<std::string_view, track_count> track_names{
    "Kick", "Bass", "Lead", "Hat"};

bool valid_waveform(int waveform) {
    return waveform >= static_cast<int>(Waveform::sine) &&
           waveform <= static_cast<int>(Waveform::noise);
}

bool pattern_has_notes(const MidiPattern& pattern) {
    return std::ranges::any_of(
        pattern.notes.begin(),
        pattern.notes.begin() + pattern.length_steps,
        [](const StepEvent& event) { return event.note >= 0; });
}

} // namespace

StudioProject StudioProject::make_demo() {
    StudioProject project;
    const Song source = Song::make_demo_song();
    project.bpm_ = source.bpm();
    for (std::size_t index = 0; index < track_count; ++index) {
        project.instruments_[index] = source.instrument(index);

        MidiPattern pattern;
        pattern.name = std::string(track_names[index]) + " 1";
        pattern.instrument = static_cast<std::uint8_t>(index);
        pattern.length_steps = 16;
        const std::size_t source_offset = index == 2 ? 16 : 0;
        for (std::size_t step = 0; step < pattern.length_steps; ++step) {
            pattern.notes[step] =
                source.step(source_offset + step).tracks[index];
        }
        project.patterns_.push_back(std::move(pattern));
    }

    for (std::size_t start = 0; start < song_step_count; start += 16) {
        for (std::size_t pattern = 0; pattern < project.patterns_.size();
             ++pattern) {
            if (pattern == 2 && start == 0) {
                continue;
            }
            project.clips_.push_back({
                .start_step = static_cast<std::uint16_t>(start),
                .lane = static_cast<std::uint8_t>(pattern),
                .pattern = static_cast<std::uint8_t>(pattern),
            });
        }
    }

    return project;
}

StudioProject StudioProject::from_song(const Song& song) {
    StudioProject project;
    project.bpm_ = song.bpm();
    for (std::size_t track = 0; track < track_count; ++track) {
        project.instruments_[track] = song.instrument(track);
    }

    for (std::size_t start = 0; start < song_step_count; start += 16) {
        for (std::size_t track = 0; track < track_count; ++track) {
            MidiPattern pattern;
            pattern.name = std::string(track_names[track]) + " " +
                           std::to_string(start / 16 + 1);
            pattern.instrument = static_cast<std::uint8_t>(track);
            pattern.length_steps = 16;
            for (std::size_t step = 0; step < 16; ++step) {
                pattern.notes[step] =
                    song.step(start + step).tracks[track];
            }
            if (!pattern_has_notes(pattern)) {
                continue;
            }

            const auto pattern_index = project.patterns_.size();
            project.patterns_.push_back(std::move(pattern));
            project.clips_.push_back({
                .start_step = static_cast<std::uint16_t>(start),
                .lane = static_cast<std::uint8_t>(track),
                .pattern = static_cast<std::uint8_t>(pattern_index),
            });
        }
    }

    if (project.patterns_.empty()) {
        MidiPattern pattern;
        pattern.name = "Pattern 1";
        project.patterns_.push_back(std::move(pattern));
    }
    return project;
}

std::uint32_t StudioProject::bpm() const {
    return bpm_;
}

void StudioProject::set_bpm(std::uint32_t bpm) {
    bpm_ = std::clamp(bpm, 40U, 300U);
}

const Instrument& StudioProject::instrument(std::size_t index) const {
    return instruments_.at(index);
}

Instrument& StudioProject::instrument(std::size_t index) {
    return instruments_.at(index);
}

std::span<const Instrument> StudioProject::instruments() const {
    return instruments_;
}

std::span<const MidiPattern> StudioProject::patterns() const {
    return patterns_;
}

std::span<MidiPattern> StudioProject::patterns() {
    return patterns_;
}

std::span<const PlaylistClip> StudioProject::clips() const {
    return clips_;
}

std::span<PlaylistClip> StudioProject::clips() {
    return clips_;
}

std::size_t StudioProject::add_pattern(MidiPattern pattern) {
    pattern.instrument =
        static_cast<std::uint8_t>(pattern.instrument % track_count);
    pattern.length_steps = std::clamp<std::uint16_t>(
        pattern.length_steps, 1, max_pattern_steps);
    patterns_.push_back(std::move(pattern));
    return patterns_.size() - 1;
}

std::size_t StudioProject::add_clip(PlaylistClip clip) {
    if (patterns_.empty()) {
        return 0;
    }
    clip.pattern = static_cast<std::uint8_t>(
        std::min<std::size_t>(clip.pattern, patterns_.size() - 1));
    clip.lane = static_cast<std::uint8_t>(
        std::min<std::size_t>(clip.lane, playlist_lane_count - 1));
    const auto length = patterns_[clip.pattern].length_steps;
    clip.start_step = static_cast<std::uint16_t>(
        std::min<std::size_t>(clip.start_step, song_step_count - length));
    clips_.push_back(clip);
    return clips_.size() - 1;
}

void StudioProject::remove_clip(std::size_t index) {
    if (index < clips_.size()) {
        clips_.erase(clips_.begin() + static_cast<std::ptrdiff_t>(index));
    }
}

bool StudioProject::lane_muted(std::size_t lane) const {
    return lane < lane_muted_.size() && lane_muted_[lane];
}

void StudioProject::set_lane_muted(std::size_t lane, bool muted) {
    if (lane < lane_muted_.size()) {
        lane_muted_[lane] = muted;
    }
}

StepRange StudioProject::populated_range() const {
    if (clips_.empty()) {
        return {};
    }

    std::size_t first = song_step_count;
    std::size_t last_exclusive = 0;
    for (const auto& clip : clips_) {
        if (clip.pattern >= patterns_.size()) {
            continue;
        }
        first = std::min<std::size_t>(first, clip.start_step);
        last_exclusive = std::max<std::size_t>(
            last_exclusive,
            clip.start_step + patterns_[clip.pattern].length_steps);
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

Song StudioProject::compile_song() const {
    Song song;
    song.set_bpm(bpm_);
    for (std::size_t index = 0; index < track_count; ++index) {
        song.instrument(index) = instruments_[index];
    }

    for (const auto& clip : clips_) {
        if (clip.pattern >= patterns_.size() || lane_muted(clip.lane)) {
            continue;
        }
        const auto& pattern = patterns_[clip.pattern];
        for (std::size_t local_step = 0;
             local_step < pattern.length_steps; ++local_step) {
            const std::size_t song_step = clip.start_step + local_step;
            if (song_step >= song_step_count) {
                break;
            }
            auto event = pattern.notes[local_step];
            if (event.note < 0) {
                continue;
            }
            event.instrument = pattern.instrument;
            song.step(song_step).tracks[pattern.instrument] = event;
        }
    }
    return song;
}

Song StudioProject::compile_pattern(std::size_t pattern_index) const {
    Song song;
    song.set_bpm(bpm_);
    for (std::size_t index = 0; index < track_count; ++index) {
        song.instrument(index) = instruments_[index];
    }
    if (pattern_index >= patterns_.size()) {
        return song;
    }

    const auto& pattern = patterns_[pattern_index];
    for (std::size_t step = 0; step < pattern.length_steps; ++step) {
        auto event = pattern.notes[step];
        if (event.note < 0) {
            continue;
        }
        event.instrument = pattern.instrument;
        song.step(step).tracks[pattern.instrument] = event;
    }
    return song;
}

bool save_studio_project(const StudioProject& project,
                         const std::filesystem::path& path,
                         std::string& error) {
    std::ofstream output(path);
    if (!output) {
        error = "Could not open studio project for writing: " + path.string();
        return false;
    }

    output << "TINY_DEMO_STUDIO " << studio_version << '\n'
           << std::setprecision(9)
           << "bpm " << project.bpm() << '\n';
    for (std::size_t index = 0; index < project.instruments().size(); ++index) {
        const auto& instrument = project.instrument(index);
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
    for (std::size_t index = 0; index < project.patterns().size(); ++index) {
        const auto& pattern = project.patterns()[index];
        output << "pattern " << index << ' ' << std::quoted(pattern.name) << ' '
               << static_cast<int>(pattern.instrument) << ' '
               << pattern.length_steps << '\n';
        for (std::size_t step = 0; step < pattern.length_steps; ++step) {
            const auto& event = pattern.notes[step];
            if (event.note < 0) {
                continue;
            }
            output << "note " << index << ' ' << step << ' '
                   << static_cast<int>(event.note) << ' '
                   << static_cast<int>(event.gate_steps) << '\n';
        }
    }
    for (const auto& clip : project.clips()) {
        output << "clip " << static_cast<int>(clip.lane) << ' '
               << clip.start_step << ' ' << static_cast<int>(clip.pattern)
               << '\n';
    }
    for (std::size_t lane = 0; lane < playlist_lane_count; ++lane) {
        output << "lane_mute " << lane << ' '
               << (project.lane_muted(lane) ? 1 : 0) << '\n';
    }
    output << "end\n";

    if (!output) {
        error = "Failed while writing studio project: " + path.string();
        return false;
    }
    error.clear();
    return true;
}

bool load_studio_project(const std::filesystem::path& path,
                         StudioProject& project, std::string& error) {
    std::ifstream input(path);
    if (!input) {
        error = "Could not open studio project: " + path.string();
        return false;
    }

    std::string magic;
    std::uint32_t version = 0;
    if (!(input >> magic >> version) || magic != "TINY_DEMO_STUDIO" ||
        version != studio_version) {
        error = "Unsupported studio project: " + path.string();
        return false;
    }

    StudioProject loaded;
    std::vector<MidiPattern> patterns;
    std::vector<PlaylistClip> clips;
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
                error = "Invalid BPM in studio project";
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
                index >= track_count || !valid_waveform(waveform)) {
                error = "Invalid instrument in studio project";
                return false;
            }
            instrument.waveform = static_cast<Waveform>(waveform);
            loaded.instrument(index) = instrument;
            continue;
        }
        if (command == "declick") {
            std::size_t index = 0;
            int enabled = 0;
            if (!(input >> index >> enabled) || index >= track_count ||
                (enabled != 0 && enabled != 1)) {
                error = "Invalid de-click setting in studio project";
                return false;
            }
            loaded.instrument(index).declick = enabled != 0;
            continue;
        }
        if (command == "lane_mute") {
            std::size_t lane = 0;
            int muted = 0;
            if (!(input >> lane >> muted) || lane >= playlist_lane_count ||
                (muted != 0 && muted != 1)) {
                error = "Invalid playlist lane mute setting";
                return false;
            }
            loaded.set_lane_muted(lane, muted != 0);
            continue;
        }
        if (command == "pattern") {
            std::size_t index = 0;
            MidiPattern pattern;
            int instrument = 0;
            if (!(input >> index >> std::quoted(pattern.name) >> instrument >>
                  pattern.length_steps) ||
                index != patterns.size() || instrument < 0 ||
                instrument >= static_cast<int>(track_count) ||
                pattern.length_steps == 0 ||
                pattern.length_steps > max_pattern_steps ||
                patterns.size() >=
                    std::numeric_limits<std::uint8_t>::max()) {
                error = "Invalid pattern in studio project";
                return false;
            }
            pattern.instrument = static_cast<std::uint8_t>(instrument);
            patterns.push_back(std::move(pattern));
            continue;
        }
        if (command == "note") {
            std::size_t pattern_index = 0;
            std::size_t step = 0;
            int note = 0;
            int gate = 0;
            if (!(input >> pattern_index >> step >> note >> gate) ||
                pattern_index >= patterns.size() ||
                step >= patterns[pattern_index].length_steps || note < 0 ||
                note > 127 || gate < 1 ||
                gate > std::numeric_limits<std::uint8_t>::max()) {
                error = "Invalid note in studio project";
                return false;
            }
            patterns[pattern_index].notes[step] = {
                .note = static_cast<std::int8_t>(note),
                .instrument = patterns[pattern_index].instrument,
                .gate_steps = static_cast<std::uint8_t>(gate),
            };
            continue;
        }
        if (command == "clip") {
            int lane = 0;
            int start = 0;
            int pattern = 0;
            if (!(input >> lane >> start >> pattern) || lane < 0 ||
                lane >= static_cast<int>(playlist_lane_count) || start < 0 ||
                start >= static_cast<int>(song_step_count) || pattern < 0 ||
                pattern >= static_cast<int>(patterns.size())) {
                error = "Invalid clip in studio project";
                return false;
            }
            clips.push_back({
                .start_step = static_cast<std::uint16_t>(start),
                .lane = static_cast<std::uint8_t>(lane),
                .pattern = static_cast<std::uint8_t>(pattern),
            });
            continue;
        }

        error = "Unknown studio project command: " + command;
        return false;
    }

    if (!reached_end || patterns.empty()) {
        error = "Studio project is incomplete";
        return false;
    }

    for (auto& pattern : patterns) {
        loaded.add_pattern(std::move(pattern));
    }
    for (const auto& clip : clips) {
        loaded.add_clip(clip);
    }
    project = std::move(loaded);
    error.clear();
    return true;
}

} // namespace tiny
