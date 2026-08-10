#include "tiny/effect.hpp"
#include "tiny/project_io.hpp"
#include "tiny/song.hpp"
#include "tiny/studio_project.hpp"
#include "tiny/synth.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string_view>
#include <vector>

namespace {

void require(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

} // namespace

int main() {
    auto song = tiny::Song::make_demo_song();

    const auto start = song.sync_at(0);
    require(start.seconds == 0.0, "the song must begin at zero seconds");
    require(start.step == 0, "the song must begin at step zero");
    require(start.pulse == 1.0F, "the beat pulse must peak at beat onset");

    const auto half_beat = song.sync_at(tiny::sample_rate / 4);
    require(std::abs(half_beat.beat_phase - 0.5F) < 0.0001F,
            "the musical clock must reach half a beat at 0.25 seconds");

    tiny::SynthEngine synth(song);
    std::vector<float> audio(tiny::sample_rate * 2);
    synth.render(audio);

    require(synth.sample_position() == tiny::sample_rate,
            "rendering one second must advance exactly one sample-rate");
    require(std::ranges::all_of(audio, [](float sample) {
                return std::isfinite(sample) && sample >= -1.0F &&
                       sample <= 1.0F;
            }),
            "synthesis must produce finite, normalized samples");

    const auto peak = std::ranges::max(
        audio, {}, [](float sample) { return std::abs(sample); });
    require(std::abs(peak) > 0.05F, "the demo song must produce audible output");

    synth.reset();
    require(synth.sample_position() == 0,
            "reset must rewind the sample position");

    tiny::SynthEngine loop_synth(tiny::Song::make_demo_song());
    loop_synth.set_loop_steps(0, 4);
    const auto loop_step_samples =
        loop_synth.song().samples_per_step();
    std::vector<float> loop_audio(
        static_cast<std::size_t>(loop_step_samples) * 5 * 2);
    loop_synth.render(loop_audio);
    require(loop_synth.sample_position() == loop_step_samples,
            "a four-step loop must wrap before rendering its fifth step");

    tiny::Song sparse_song;
    sparse_song.step(0).tracks[0].note = 36;
    sparse_song.step(63).tracks[1].note = 48;
    const auto sparse_range = sparse_song.populated_range();
    require(sparse_range.start == 0 && sparse_range.length == 64,
            "populated song steps must round outward to complete bars");
    const auto sparse_studio = tiny::StudioProject::from_song(sparse_song);
    const auto sparse_playlist_range = sparse_studio.populated_range();
    require(sparse_playlist_range.start == 0 &&
                sparse_playlist_range.length == 64,
            "playlist clips in bars one through four must auto-loop four bars");

    song.set_bpm(137);
    song.instrument(2).filter_cutoff = 0.456789F;
    song.instrument(2).declick = false;
    song.step(7).tracks[2] = {
        .note = 71,
        .instrument = 2,
        .gate_steps = 3,
    };

    const auto temporary = std::filesystem::temp_directory_path();
    const auto project_path = temporary / "tiny-demo-core-test.tds";
    const auto binary_path = temporary / "tiny-demo-core-test.bin";
    const auto shader_path = temporary / "tiny-demo-effect-test.frag";
    const auto preset_path = temporary / "tiny-demo-effect-test.fxp";
    const auto studio_path = temporary / "tiny-demo-studio-test.tds";
    std::filesystem::remove(project_path);
    std::filesystem::remove(binary_path);
    std::filesystem::remove(shader_path);
    std::filesystem::remove(preset_path);
    std::filesystem::remove(studio_path);

    std::string error;
    require(tiny::save_song_project(song, project_path, error), error);
    tiny::Song project_copy;
    require(tiny::load_song_project(project_path, project_copy, error), error);
    require(project_copy.bpm() == 137, "project BPM must round-trip");
    require(project_copy.step(7).tracks[2].note == 71,
            "project notes must round-trip");
    require(std::abs(project_copy.instrument(2).filter_cutoff - 0.456789F) <
                0.000001F,
            "project instrument values must round-trip");
    require(!project_copy.instrument(2).declick,
            "project de-click settings must round-trip");

    require(tiny::export_song_binary(project_copy, binary_path, error), error);
    tiny::Song binary_copy;
    require(tiny::load_song_binary(binary_path, binary_copy, error), error);
    require(binary_copy.bpm() == 137, "binary BPM must round-trip");
    require(binary_copy.step(7).tracks[2].gate_steps == 3,
            "binary note gates must round-trip");
    require(binary_copy.instrument(2).filter_cutoff ==
                project_copy.instrument(2).filter_cutoff,
            "binary instrument values must round-trip exactly");
    require(!binary_copy.instrument(2).declick,
            "binary de-click settings must round-trip");

    {
        std::ofstream shader(shader_path);
        shader << "#version 330 core\n"
               << "// @param u_speed \"Camera speed\" 1.0 0.1 3.0 0.1\n"
               << "uniform float u_speed;\n"
               << "// @param u_size \"Star size\" 0.5 0.1 2.0 0.05\n"
               << "uniform float u_size;\n";
    }
    tiny::EffectSettings effect;
    require(effect.load_schema(shader_path, false, error), error);
    require(effect.parameters().size() == 2,
            "shader annotations must generate two parameters");
    require(effect.parameters()[0].label == "Camera speed",
            "effect parameter labels must parse");
    effect.parameters()[0].value = 2.3F;
    effect.add_text({
        .text = "HELLO AMIGA",
        .x = 0.25F,
        .y = 0.40F,
        .scale = 6.0F,
        .color = {0.2F, 0.8F, 1.0F},
        .enabled = true,
    });
    effect.add_clip({
        .name = "STARFIELD",
        .start_step = 16,
        .length_steps = 32,
        .enabled = true,
    });
    require(effect.save_preset(preset_path, error), error);
    effect.reset();
    require(effect.parameters()[0].value == 1.0F,
            "effect reset must restore the shader default");
    require(effect.load_preset(preset_path, error), error);
    require(std::abs(effect.parameters()[0].value - 2.3F) < 0.000001F,
            "effect preset values must round-trip");
    require(effect.texts().size() == 1 &&
                effect.texts()[0].text == "HELLO AMIGA",
            "effect text overlays must round-trip");
    require(std::abs(effect.texts()[0].x - 0.25F) < 0.000001F &&
                std::abs(effect.texts()[0].color[1] - 0.8F) < 0.000001F,
            "effect text placement and color must round-trip");
    require(effect.clips().size() == 1 &&
                effect.clips()[0].start_step == 16 &&
                effect.clips()[0].length_steps == 32,
            "visual effect clips must round-trip");
    require(!effect.active_at(15) && effect.active_at(16) &&
                !effect.active_at(48),
            "visual effect clips must gate shader playback by song step");
    tiny::TextOverlay centered{
        .text = "AB",
        .scale = 5.0F,
    };
    require(std::abs(tiny::text_pixel_size_for_viewport(
                         centered.scale, 640.0F, 360.0F) -
                     2.5F) <
                0.000001F,
            "preview text pixels must scale down with the viewport");
    require(std::abs(tiny::text_pixel_size_for_viewport(
                         centered.scale, 1280.0F, 720.0F) -
                     5.0F) <
                0.000001F,
            "text scale must describe pixels on the demo canvas");
    tiny::center_text_overlay(centered);
    require(std::abs(centered.x - 0.4765625F) < 0.000001F &&
                std::abs(centered.y - 0.4722222F) < 0.000001F,
            "pixel text must center using its demo-canvas bounds");

    auto studio = tiny::StudioProject::make_demo();
    studio.instrument(0).declick = false;
    require(studio.patterns().size() == 4,
            "the demo studio project must have reusable instrument patterns");
    require(studio.clips().size() > studio.patterns().size(),
            "the playlist must reuse patterns across multiple clips");
    studio.patterns()[0].notes[0].note = 48;
    const auto arranged = studio.compile_song();
    require(arranged.step(0).tracks[0].note == 48,
            "the first pattern clip must compile into the runtime song");
    require(arranged.step(16).tracks[0].note == 48,
            "editing a pattern must affect every reused playlist clip");
    studio.set_lane_muted(0, true);
    require(studio.compile_song().step(16).tracks[0].note < 0,
            "muting a playlist lane must silence clips on that lane");
    require(studio.compile_pattern(0).step(0).tracks[0].note == 48,
            "playlist lane mute must not silence pattern solo preview");
    require(tiny::save_studio_project(studio, studio_path, error), error);
    tiny::StudioProject studio_copy;
    require(tiny::load_studio_project(studio_path, studio_copy, error), error);
    require(studio_copy.patterns().size() == studio.patterns().size(),
            "studio patterns must round-trip");
    require(studio_copy.clips().size() == studio.clips().size(),
            "playlist clips must round-trip");
    require(!studio_copy.instrument(0).declick,
            "studio instrument de-click settings must round-trip");
    require(studio_copy.lane_muted(0),
            "playlist lane mute settings must round-trip");
    require(studio_copy.compile_song().step(16).tracks[0].note < 0,
            "a loaded muted playlist lane must remain silent");
    const auto isolated_pattern = studio_copy.compile_pattern(0);
    require(isolated_pattern.step(0).tracks[0].note == 48,
            "pattern preview must contain the selected pattern");
    require(isolated_pattern.step(0).tracks[1].note < 0,
            "pattern preview must exclude other instruments");

    std::filesystem::remove(project_path);
    std::filesystem::remove(binary_path);
    std::filesystem::remove(shader_path);
    std::filesystem::remove(preset_path);
    std::filesystem::remove(studio_path);

    std::cout << "tiny_core: synthesis and musical clock are healthy\n";
    return 0;
}
