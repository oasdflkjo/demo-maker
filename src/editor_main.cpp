#include "editor_ui.hpp"
#include "tiny/project_io.hpp"
#include "tiny/studio_project.hpp"
#include "tiny/synth.hpp"

#include <SDL2/SDL.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using tiny::editor::Rect;

constexpr SDL_Color background{18, 20, 29, 255};
constexpr SDL_Color panel{27, 30, 43, 255};
constexpr SDL_Color panel_alt{32, 35, 50, 255};
constexpr SDL_Color foreground{226, 229, 241, 255};
constexpr SDL_Color muted{123, 129, 154, 255};
constexpr SDL_Color accent{119, 96, 238, 255};
constexpr SDL_Color playhead{47, 171, 145, 160};
constexpr SDL_Color warning{242, 184, 92, 255};

constexpr std::array<std::string_view, tiny::track_count> track_names{
    "KICK", "BASS", "LEAD", "HAT"};
constexpr std::array<std::string_view, 5> waveform_names{
    "SINE", "TRIANGLE", "SAW", "PULSE", "NOISE"};
constexpr std::array<SDL_Color, tiny::track_count> pattern_colors{
    SDL_Color{226, 92, 104, 255},
    SDL_Color{53, 184, 170, 255},
    SDL_Color{130, 105, 238, 255},
    SDL_Color{225, 177, 73, 255},
};

enum class EditorView {
    playlist,
    piano_roll,
};

struct ClipDragOrigin {
    std::size_t index{0};
    std::uint16_t start_step{0};
    std::uint8_t lane{0};
};

struct AudioState {
    explicit AudioState(tiny::Song song) : synth(std::move(song)) {}

    tiny::SynthEngine synth;
    std::atomic<std::uint64_t> generated_samples{0};
    std::atomic<std::uint64_t> rendered_samples{0};
};

struct EditorState {
    EditorView view{EditorView::playlist};
    std::size_t selected_pattern{0};
    std::optional<std::size_t> selected_clip;
    std::vector<std::size_t> selected_clips;
    std::size_t selected_step{0};
    int selected_note{36};
    std::array<int, tiny::track_count> view_octaves{2, 2, 4, 6};
    std::array<float, tiny::track_count> piano_top_notes{59, 59, 83, 107};
    float piano_horizontal_zoom{1.0F};
    float piano_vertical_zoom{1.0F};
    float piano_scroll_step{0.0F};
    bool dragging_horizontal_scrollbar{false};
    bool dragging_vertical_scrollbar{false};
    float scrollbar_drag_offset{0.0F};
    std::size_t playlist_resume_step{0};
    std::vector<ClipDragOrigin> dragging_clips;
    std::size_t drag_pointer_step{0};
    std::size_t drag_pointer_lane{0};
    std::optional<std::size_t> dragging_pattern;
    bool box_selecting_clips{false};
    float box_selection_start_x{0.0F};
    float box_selection_start_y{0.0F};
    std::vector<std::size_t> box_selection_base;
    bool playing{true};
    std::string status{
        "Drag the pattern block into the playlist. Double-click a clip to edit."};
    bool status_is_error{false};
};

bool clip_is_selected(const EditorState& state, std::size_t index) {
    return std::ranges::find(state.selected_clips, index) !=
           state.selected_clips.end();
}

void select_only_clip(EditorState& state, std::size_t index) {
    state.selected_clip = index;
    state.selected_clips = {index};
}

bool rectangles_intersect(Rect first, Rect second) {
    return first.x < second.x + second.width &&
           first.x + first.width > second.x &&
           first.y < second.y + second.height &&
           first.y + first.height > second.y;
}

Rect rectangle_between(float start_x, float start_y, float end_x,
                       float end_y) {
    return {
        .x = std::min(start_x, end_x),
        .y = std::min(start_y, end_y),
        .width = std::abs(end_x - start_x),
        .height = std::abs(end_y - start_y),
    };
}

void audio_callback(void* user_data, std::uint8_t* stream, int byte_count) {
    auto& state = *static_cast<AudioState*>(user_data);
    auto* samples = reinterpret_cast<float*>(stream);
    const auto sample_count =
        static_cast<std::size_t>(byte_count) / sizeof(float);
    state.synth.render(std::span<float>(samples, sample_count));
    state.generated_samples.store(state.synth.sample_position(),
                                  std::memory_order_release);
    state.rendered_samples.fetch_add(sample_count / 2,
                                     std::memory_order_release);
}

std::uint64_t audible_loop_position(std::uint64_t generated,
                                    std::uint64_t rendered,
                                    std::uint64_t latency,
                                    std::uint64_t samples_per_step,
                                    tiny::StepRange loop) {
    const auto audible_delay = std::min(latency, rendered);
    if (audible_delay == 0) {
        return generated;
    }

    const auto loop_start =
        static_cast<std::uint64_t>(loop.start) * samples_per_step;
    const auto loop_length =
        static_cast<std::uint64_t>(loop.length) * samples_per_step;
    const auto loop_end = loop_start + loop_length;
    if (loop_length > 0 && generated >= loop_start &&
        generated <= loop_end) {
        const auto generated_offset =
            (generated - loop_start) % loop_length;
        const auto delay_in_loop = audible_delay % loop_length;
        const auto audible_offset =
            (generated_offset + loop_length - delay_in_loop) % loop_length;
        return loop_start + audible_offset;
    }

    return generated > audible_delay ? generated - audible_delay : 0;
}

std::filesystem::path find_font() {
    constexpr std::array candidates{
        "/usr/share/fonts/adwaita-mono-fonts/AdwaitaMono-Regular.ttf",
        "/usr/share/fonts/abattis-cantarell-fonts/Cantarell-Regular.otf",
        "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    };
    for (const auto* candidate : candidates) {
        if (std::filesystem::exists(candidate)) {
            return candidate;
        }
    }
    return {};
}

std::string note_name(int note) {
    if (note < 0) {
        return "---";
    }
    constexpr std::array names{"C-", "C#", "D-", "D#", "E-", "F-",
                               "F#", "G-", "G#", "A-", "A#", "B-"};
    return std::string(names[static_cast<std::size_t>(note % 12)]) +
           std::to_string(note / 12 - 1);
}

std::optional<int> piano_key(SDL_Keycode key) {
    switch (key) {
    case SDLK_z:
        return 0;
    case SDLK_s:
        return 1;
    case SDLK_x:
        return 2;
    case SDLK_d:
        return 3;
    case SDLK_c:
        return 4;
    case SDLK_v:
        return 5;
    case SDLK_g:
        return 6;
    case SDLK_b:
        return 7;
    case SDLK_h:
        return 8;
    case SDLK_n:
        return 9;
    case SDLK_j:
        return 10;
    case SDLK_m:
        return 11;
    case SDLK_COMMA:
        return 12;
    case SDLK_l:
        return 13;
    case SDLK_PERIOD:
        return 14;
    default:
        return std::nullopt;
    }
}

std::uint8_t default_gate(std::size_t instrument) {
    return instrument == 1 || instrument == 2 ? 2 : 1;
}

void apply_playlist_audio(SDL_AudioDeviceID device, AudioState& audio,
                          const tiny::StudioProject& project,
                          bool reset_transport,
                          std::optional<std::size_t> seek_step = std::nullopt) {
    const auto loop = project.populated_range();
    SDL_LockAudioDevice(device);
    audio.synth.clear_loop();
    audio.synth.set_song(project.compile_song(), reset_transport);
    if (seek_step.has_value()) {
        audio.synth.seek_to_step(*seek_step);
    }
    audio.synth.set_loop_steps(loop.start, loop.length);
    audio.generated_samples.store(audio.synth.sample_position(),
                                  std::memory_order_release);
    if (reset_transport || seek_step.has_value()) {
        audio.rendered_samples.store(0, std::memory_order_release);
    }
    SDL_UnlockAudioDevice(device);
}

void apply_pattern_audio(SDL_AudioDeviceID device, AudioState& audio,
                         const tiny::StudioProject& project,
                         std::size_t pattern_index, bool reset_transport) {
    const auto pattern_length =
        project.patterns()[pattern_index].length_steps;
    SDL_LockAudioDevice(device);
    audio.synth.clear_loop();
    audio.synth.set_song(project.compile_pattern(pattern_index),
                         reset_transport);
    audio.synth.set_loop_steps(0, pattern_length);
    if (reset_transport) {
        audio.synth.seek_to_step(0);
        audio.rendered_samples.store(0, std::memory_order_release);
    }
    audio.generated_samples.store(audio.synth.sample_position(),
                                  std::memory_order_release);
    SDL_UnlockAudioDevice(device);
}

void audition(SDL_AudioDeviceID device, AudioState& audio, int note,
              std::size_t instrument) {
    SDL_LockAudioDevice(device);
    audio.synth.audition(note, instrument);
    SDL_UnlockAudioDevice(device);
}

void save_project(const tiny::StudioProject& project,
                  const std::filesystem::path& project_path,
                  EditorState& state) {
    std::string error;
    if (tiny::save_studio_project(project, project_path, error)) {
        state.status = "Saved " + project_path.string();
        state.status_is_error = false;
    } else {
        state.status = error;
        state.status_is_error = true;
    }
}

bool load_project(tiny::StudioProject& project,
                  const std::filesystem::path& project_path,
                  EditorState& state) {
    std::string studio_error;
    tiny::StudioProject loaded;
    if (tiny::load_studio_project(project_path, loaded, studio_error)) {
        project = std::move(loaded);
        state.status = "Loaded " + project_path.string();
        state.status_is_error = false;
        return true;
    }

    tiny::Song legacy;
    std::string legacy_error;
    if (tiny::load_song_project(project_path, legacy, legacy_error)) {
        project = tiny::StudioProject::from_song(legacy);
        state.status = "Imported legacy song as reusable playlist patterns";
        state.status_is_error = false;
        return true;
    }

    state.status = studio_error;
    state.status_is_error = true;
    return false;
}

void export_project(const tiny::StudioProject& project,
                    const std::filesystem::path& project_path,
                    EditorState& state) {
    auto export_path = project_path;
    export_path.replace_extension(".bin");
    std::string error;
    if (tiny::export_song_binary(project.compile_song(), export_path, error)) {
        state.status = "Exported flattened runtime song " + export_path.string();
        state.status_is_error = false;
    } else {
        state.status = error;
        state.status_is_error = true;
    }
}

void print_help(std::string_view executable) {
    std::cout << "Usage: " << executable
              << " [--project FILE] [--font FILE]\n";
}

bool draw_instrument_panel(tiny::editor::Ui& ui,
                           tiny::StudioProject& project,
                           const tiny::MidiPattern& pattern, float panel_x,
                           float panel_width) {
    bool changed = false;
    const std::size_t instrument_index = pattern.instrument;
    auto& instrument = project.instrument(instrument_index);

    ui.fill({panel_x, 88, panel_width, 720}, panel);
    ui.text(panel_x + 18, 106,
            "INSTRUMENT " + std::to_string(instrument_index + 1) + " / " +
                std::string(track_names[instrument_index]),
            foreground);

    const auto waveform_index =
        static_cast<std::size_t>(instrument.waveform);
    if (ui.button({panel_x + 18, 138, panel_width - 36, 36},
                  "WAVE  " + std::string(waveform_names[waveform_index]))) {
        instrument.waveform = static_cast<tiny::Waveform>(
            (waveform_index + 1) % waveform_names.size());
        changed = true;
    }

    const float slider_x = panel_x + 18;
    const float slider_width = panel_width - 36;
    float slider_y = 193;
    int slider_id = 200;
    auto instrument_slider =
        [&](std::string_view label, float& value, float minimum,
            float maximum, int decimals = 2) {
            const bool slider_changed =
                ui.slider(slider_id++, {slider_x, slider_y, slider_width, 40},
                          label, value, minimum, maximum, decimals);
            slider_y += 56;
            return slider_changed;
        };

    changed |= instrument_slider(
        "ATTACK", instrument.attack_seconds, 0.001F, 1.0F, 3);
    changed |= instrument_slider(
        "DECAY", instrument.decay_seconds, 0.01F, 2.0F);
    changed |= instrument_slider("SUSTAIN", instrument.sustain, 0.0F, 1.0F);
    changed |= instrument_slider(
        "RELEASE", instrument.release_seconds, 0.01F, 2.0F);
    changed |= instrument_slider(
        "PULSE WIDTH", instrument.pulse_width, 0.05F, 0.95F);
    changed |= instrument_slider(
        "FILTER CUTOFF", instrument.filter_cutoff, 0.01F, 1.0F);
    changed |= instrument_slider(
        "RESONANCE", instrument.filter_resonance, 0.0F, 0.95F);
    changed |= instrument_slider(
        "PITCH DROP", instrument.pitch_drop_semitones, 0.0F, 48.0F, 1);
    changed |= instrument_slider("GAIN", instrument.gain, 0.0F, 1.0F);
    changed |= instrument_slider("PAN", instrument.pan, -1.0F, 1.0F);
    changed |= ui.checkbox(
        {slider_x, slider_y - 2, slider_width, 36},
        "REMOVE END CLICKS", instrument.declick);
    return changed;
}

} // namespace

int main(int argc, char** argv) {
    std::filesystem::path project_path = "song.tds";
    std::filesystem::path font_path = find_font();

    for (int index = 1; index < argc; ++index) {
        const std::string_view argument = argv[index];
        if (argument == "--help" || argument == "-h") {
            print_help(argv[0]);
            return 0;
        }
        if (argument == "--project" && index + 1 < argc) {
            project_path = argv[++index];
            continue;
        }
        if (argument == "--font" && index + 1 < argc) {
            font_path = argv[++index];
            continue;
        }
        std::cerr << "Unknown or incomplete argument: " << argument << '\n';
        print_help(argv[0]);
        return 1;
    }

    if (font_path.empty()) {
        std::cerr << "No usable editor font was found. Pass --font FILE.\n";
        return 1;
    }

    tiny::StudioProject project = tiny::StudioProject::make_demo();
    EditorState state;
    if (std::filesystem::exists(project_path)) {
        load_project(project, project_path, state);
    }
    state.selected_pattern =
        std::min(state.selected_pattern, project.patterns().size() - 1);

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0) {
        std::cerr << "SDL initialization failed: " << SDL_GetError() << '\n';
        return 1;
    }

    SDL_Window* window = SDL_CreateWindow(
        "Demo Maker — Playlist", SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED, 1440, 900,
        SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (window == nullptr) {
        std::cerr << "Window creation failed: " << SDL_GetError() << '\n';
        SDL_Quit();
        return 1;
    }
    SDL_SetWindowMinimumSize(window, 1200, 860);

    SDL_Renderer* renderer =
        SDL_CreateRenderer(window, -1,
                           SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (renderer == nullptr) {
        renderer = SDL_CreateRenderer(window, -1, 0);
    }
    if (renderer == nullptr) {
        std::cerr << "Renderer creation failed: " << SDL_GetError() << '\n';
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    tiny::editor::Ui ui;
    std::string ui_error;
    if (!ui.initialize(renderer, font_path, ui_error)) {
        std::cerr << ui_error << '\n';
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    AudioState audio(project.compile_song());
    const auto initial_playlist_loop = project.populated_range();
    audio.synth.set_loop_steps(initial_playlist_loop.start,
                               initial_playlist_loop.length);
    SDL_AudioSpec desired{};
    desired.freq = static_cast<int>(tiny::sample_rate);
    desired.format = AUDIO_F32SYS;
    desired.channels = 2;
    desired.samples = 512;
    desired.callback = audio_callback;
    desired.userdata = &audio;

    SDL_AudioSpec obtained{};
    const SDL_AudioDeviceID audio_device =
        SDL_OpenAudioDevice(nullptr, 0, &desired, &obtained, 0);
    if (audio_device == 0) {
        std::cerr << "Audio device creation failed: " << SDL_GetError() << '\n';
        ui.shutdown();
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    SDL_PauseAudioDevice(audio_device, 0);

    bool running = true;
    while (running) {
        tiny::editor::Input input;
        const auto mouse_buttons =
            SDL_GetMouseState(&input.mouse_x, &input.mouse_y);
        input.mouse_down = (mouse_buttons & SDL_BUTTON_LMASK) != 0;
        input.right_down = (mouse_buttons & SDL_BUTTON_RMASK) != 0;
        input.control_down = (SDL_GetModState() & KMOD_CTRL) != 0;
        input.shift_down = (SDL_GetModState() & KMOD_SHIFT) != 0;

        bool project_changed = false;
        bool reset_transport = false;
        bool toggle_playback = false;
        bool stop_playback = false;
        bool request_save = false;
        bool request_load = false;
        bool request_export = false;
        bool switch_to_pattern_audio = false;
        bool switch_to_playlist_audio = false;
        std::optional<int> audition_note;

        SDL_Event event{};
        while (SDL_PollEvent(&event) != 0) {
            if (event.type == SDL_QUIT) {
                running = false;
            } else if (event.type == SDL_MOUSEMOTION) {
                input.mouse_x = event.motion.x;
                input.mouse_y = event.motion.y;
            } else if (event.type == SDL_MOUSEBUTTONDOWN) {
                input.mouse_x = event.button.x;
                input.mouse_y = event.button.y;
                if (event.button.button == SDL_BUTTON_LEFT) {
                    input.mouse_pressed = true;
                    input.mouse_double_clicked = event.button.clicks >= 2;
                    input.mouse_down = true;
                } else if (event.button.button == SDL_BUTTON_RIGHT) {
                    input.right_pressed = true;
                    input.right_down = true;
                }
            } else if (event.type == SDL_MOUSEBUTTONUP) {
                input.mouse_x = event.button.x;
                input.mouse_y = event.button.y;
                if (event.button.button == SDL_BUTTON_LEFT) {
                    input.mouse_released = true;
                    input.mouse_down = false;
                } else if (event.button.button == SDL_BUTTON_RIGHT) {
                    input.right_down = false;
                }
            } else if (event.type == SDL_MOUSEWHEEL) {
                input.wheel_y += event.wheel.y;
            } else if (event.type == SDL_KEYDOWN) {
                const auto key = event.key.keysym.sym;
                const auto modifiers =
                    static_cast<SDL_Keymod>(event.key.keysym.mod);
                const bool control = (modifiers & KMOD_CTRL) != 0;

                if (key == SDLK_ESCAPE) {
                    if (state.view == EditorView::piano_roll) {
                        state.view = EditorView::playlist;
                        switch_to_playlist_audio = true;
                        state.status =
                            "Playlist playback restored at the previous position.";
                        state.status_is_error = false;
                        SDL_SetWindowTitle(window,
                                           "Demo Maker — Playlist");
                    } else {
                        running = false;
                    }
                } else if (control && key == SDLK_s) {
                    request_save = true;
                } else if (control && key == SDLK_o) {
                    request_load = true;
                } else if (control && key == SDLK_e) {
                    request_export = true;
                } else if (key == SDLK_SPACE && event.key.repeat == 0) {
                    toggle_playback = true;
                } else if (state.view == EditorView::piano_roll) {
                    auto& pattern =
                        project.patterns()[state.selected_pattern];
                    const auto instrument = pattern.instrument;
                    if (key == SDLK_LEFT) {
                        state.selected_step =
                            (state.selected_step + pattern.length_steps - 1) %
                            pattern.length_steps;
                    } else if (key == SDLK_RIGHT) {
                        state.selected_step =
                            (state.selected_step + 1) % pattern.length_steps;
                    } else if (key == SDLK_UP || key == SDLK_DOWN) {
                        auto& selected = pattern.notes[state.selected_step];
                        const int base_note =
                            selected.note < 0
                                ? (state.view_octaves[instrument] + 1) * 12
                                : selected.note;
                        selected.note = static_cast<std::int8_t>(std::clamp(
                            base_note + (key == SDLK_UP ? 1 : -1), 0, 127));
                        selected.instrument = instrument;
                        selected.gate_steps = default_gate(instrument);
                        state.selected_note = selected.note;
                        audition_note = selected.note;
                        project_changed = true;
                    } else if (key == SDLK_DELETE || key == SDLK_BACKSPACE) {
                        pattern.notes[state.selected_step] = {};
                        project_changed = true;
                    } else if (key == SDLK_LEFTBRACKET) {
                        state.view_octaves[instrument] =
                            std::max(0, state.view_octaves[instrument] - 1);
                    } else if (key == SDLK_RIGHTBRACKET) {
                        state.view_octaves[instrument] =
                            std::min(8, state.view_octaves[instrument] + 1);
                    } else if (!control && event.key.repeat == 0) {
                        if (const auto semitone = piano_key(key);
                            semitone.has_value()) {
                            const int note = std::clamp(
                                (state.view_octaves[instrument] + 1) * 12 +
                                    *semitone,
                                0, 127);
                            pattern.notes[state.selected_step] = {
                                .note = static_cast<std::int8_t>(note),
                                .instrument = instrument,
                                .gate_steps = default_gate(instrument),
                            };
                            state.selected_note = note;
                            audition_note = note;
                            state.selected_step =
                                (state.selected_step + 1) %
                                pattern.length_steps;
                            project_changed = true;
                        }
                    }
                }
            }
        }

        int window_width = 0;
        int window_height = 0;
        SDL_GetWindowSize(window, &window_width, &window_height);
        SDL_RenderSetLogicalSize(renderer, window_width, window_height);
        ui.begin_frame(input);

        SDL_SetRenderDrawColor(renderer, background.r, background.g,
                               background.b, background.a);
        SDL_RenderClear(renderer);
        ui.fill({0, 0, static_cast<float>(window_width), 72}, panel);
        ui.text(24, 20, "DEMO MAKER", foreground);
        ui.text(24, 43,
                state.view == EditorView::playlist ? "PLAYLIST / ARRANGEMENT"
                                                   : "PATTERN PIANO ROLL",
                muted);

        if (ui.button({250, 17, 86, 38},
                      state.playing ? "PAUSE" : "PLAY", state.playing)) {
            toggle_playback = true;
        }
        if (ui.button({346, 17, 72, 38}, "STOP")) {
            stop_playback = true;
        }
        if (ui.button({444, 17, 72, 38}, "SAVE")) {
            request_save = true;
        }
        if (ui.button({526, 17, 72, 38}, "LOAD")) {
            request_load = true;
        }
        if (ui.button({608, 17, 92, 38}, "EXPORT")) {
            request_export = true;
        }

        float bpm_value = static_cast<float>(project.bpm());
        if (ui.slider(100, {735, 11, 190, 40}, "BPM", bpm_value, 60.0F,
                      200.0F, 0)) {
            project.set_bpm(
                static_cast<std::uint32_t>(std::lround(bpm_value)));
            project_changed = true;
        }

        const auto rendered =
            audio.rendered_samples.load(std::memory_order_acquire);
        const auto generated =
            audio.generated_samples.load(std::memory_order_acquire);
        const auto latency = static_cast<std::uint64_t>(obtained.samples);
        const tiny::Song flattened = project.compile_song();
        const tiny::StepRange active_loop =
            state.view == EditorView::playlist
                ? project.populated_range()
                : tiny::StepRange{
                      .start = 0,
                      .length =
                          project.patterns()[state.selected_pattern]
                              .length_steps,
                  };
        const auto audible_position = audible_loop_position(
            generated, rendered, latency, flattened.samples_per_step(),
            active_loop);
        const auto sync = flattened.sync_at(audible_position);
        ui.text(965, 24,
                "STEP " + std::to_string(sync.step + 1) + "  BEAT " +
                    std::to_string(static_cast<int>(sync.beat) + 1),
                playhead);

        const float editor_right =
            std::max(720.0F, static_cast<float>(window_width) - 410.0F);
        const float panel_x = editor_right + 10.0F;
        const float panel_width =
            static_cast<float>(window_width) - panel_x - 24.0F;

        auto& selected_pattern =
            project.patterns()[state.selected_pattern];

        if (state.view == EditorView::playlist) {
            ui.text(24, 91, "PATTERN BLOCK", muted);
            if (ui.button({148, 81, 34, 34}, "<")) {
                state.selected_pattern =
                    (state.selected_pattern + project.patterns().size() - 1) %
                    project.patterns().size();
            }
            const Rect pattern_tile{190, 81, 226, 34};
            ui.fill(pattern_tile,
                    pattern_colors[selected_pattern.instrument]);
            ui.text(pattern_tile.x + 10, pattern_tile.y + 7,
                    std::to_string(state.selected_pattern + 1) + "  " +
                        selected_pattern.name,
                    SDL_Color{20, 21, 28, 255});
            ui.outline(pattern_tile, SDL_Color{235, 236, 245, 255});
            if (input.mouse_pressed && ui.hovered(pattern_tile)) {
                state.dragging_pattern = state.selected_pattern;
            }
            if (ui.button({426, 81, 34, 34}, ">")) {
                state.selected_pattern =
                    (state.selected_pattern + 1) % project.patterns().size();
            }
            if (ui.button({480, 81, 68, 34}, "NEW")) {
                tiny::MidiPattern pattern;
                pattern.name =
                    "Pattern " + std::to_string(project.patterns().size() + 1);
                pattern.instrument = selected_pattern.instrument;
                state.selected_pattern =
                    project.add_pattern(std::move(pattern));
                project_changed = true;
            }
            if (ui.button({558, 81, 94, 34}, "DUPLICATE")) {
                auto duplicate = selected_pattern;
                duplicate.name =
                    "Pattern " + std::to_string(project.patterns().size() + 1);
                state.selected_pattern =
                    project.add_pattern(std::move(duplicate));
                project_changed = true;
            }
            if (ui.button({662, 81, 120, 34}, "EDIT PATTERN")) {
                state.playlist_resume_step =
                    static_cast<std::size_t>(
                        audible_position / flattened.samples_per_step()) %
                    tiny::song_step_count;
                state.view = EditorView::piano_roll;
                switch_to_pattern_audio = true;
                state.status =
                    "Pattern is playing solo and looping while you edit.";
                state.status_is_error = false;
                state.selected_step = 0;
                state.selected_note =
                    (state.view_octaves[selected_pattern.instrument] + 1) * 12;
                SDL_SetWindowTitle(window,
                                   "Demo Maker — Pattern Piano Roll");
            }

            ui.text(24, 141, "PLAYLIST", foreground);
            ui.text(116, 141,
                    "WHOLE SONG  BARS 1-" +
                        std::to_string(tiny::song_step_count /
                                       tiny::steps_per_bar),
                    muted);
            const auto playlist_loop = project.populated_range();
            ui.text(350, 141,
                    "AUTO LOOP  " +
                        std::to_string(playlist_loop.start /
                                           tiny::steps_per_bar +
                                       1) +
                        "-" +
                        std::to_string((playlist_loop.start +
                                        playlist_loop.length) /
                                       tiny::steps_per_bar),
                    playhead);
            if (!state.selected_clips.empty()) {
                ui.text(570, 141,
                        std::to_string(state.selected_clips.size()) +
                            (state.selected_clips.size() == 1
                                 ? " CLIP SELECTED"
                                 : " CLIPS SELECTED"),
                        accent);
            }

            constexpr std::size_t visible_steps = tiny::song_step_count;
            constexpr float lane_height = 61.0F;
            const float grid_x = 132.0F;
            const float grid_y = 198.0F;
            const float grid_width =
                std::max(540.0F, editor_right - grid_x - 20);
            const float step_width = grid_width / visible_steps;
            const Rect playlist_rect{
                grid_x,
                grid_y,
                grid_width,
                lane_height * tiny::playlist_lane_count,
            };

            for (std::size_t lane = 0; lane < tiny::playlist_lane_count;
                 ++lane) {
                const float y = grid_y + static_cast<float>(lane) * lane_height;
                const bool is_muted = project.lane_muted(lane);
                ui.text(16, y + 20, "L" + std::to_string(lane + 1),
                        is_muted ? warning : muted);
                bool next_muted = is_muted;
                if (ui.checkbox({48, y + 13, 80, 34}, "MUTE",
                                next_muted)) {
                    project.set_lane_muted(lane, next_muted);
                    state.status =
                        "Lane " + std::to_string(lane + 1) +
                        (next_muted ? " muted." : " unmuted.");
                    state.status_is_error = false;
                    project_changed = true;
                }
                ui.fill({grid_x, y, grid_width, lane_height - 2},
                        is_muted
                            ? SDL_Color{24, 26, 36, 255}
                            : lane % 2 == 0
                                  ? panel_alt
                                  : SDL_Color{29, 32, 46, 255});
                for (std::size_t step = 0; step < visible_steps; step += 4) {
                    const float x =
                        grid_x + static_cast<float>(step) * step_width;
                    ui.fill({x, y, step % 16 == 0 ? 2.0F : 1.0F,
                             lane_height - 2},
                            step % 16 == 0
                                ? SDL_Color{88, 91, 116, 255}
                                : SDL_Color{55, 59, 78, 255});
                }
            }
            for (std::size_t bar = 0;
                 bar < tiny::song_step_count / tiny::steps_per_bar; ++bar) {
                ui.text(grid_x + static_cast<float>(bar * 16) * step_width + 6,
                        grid_y - 25,
                        std::to_string(bar + 1),
                        foreground);
            }
            const double playhead_step =
                static_cast<double>(audible_position) /
                static_cast<double>(flattened.samples_per_step());

            auto mouse_playlist_position =
                [&]() -> std::optional<std::pair<std::size_t, std::size_t>> {
                if (!ui.hovered(playlist_rect)) {
                    return std::nullopt;
                }
                const auto step = static_cast<std::size_t>(
                    (input.mouse_x - grid_x) / step_width);
                const auto lane = static_cast<std::size_t>(
                    (input.mouse_y - grid_y) / lane_height);
                return std::pair{
                    std::min(step, tiny::song_step_count - 1),
                    std::min(lane, tiny::playlist_lane_count - 1),
                };
            };

            std::optional<std::size_t> delete_clip;
            std::optional<std::size_t> begin_drag_clip;
            bool copy_drag_selection = false;
            std::vector<std::pair<std::size_t, Rect>> clip_blocks;
            bool pointer_over_clip = false;
            for (std::size_t index = 0; index < project.clips().size();
                ++index) {
                const auto& clip = project.clips()[index];
                const auto& pattern = project.patterns()[clip.pattern];
                const float x =
                    grid_x +
                    static_cast<float>(clip.start_step) * step_width;
                const float width =
                    std::max(2.0F,
                             static_cast<float>(pattern.length_steps) *
                                     step_width -
                                 3);
                const Rect block{
                    x + 2,
                    grid_y + static_cast<float>(clip.lane) * lane_height + 5,
                    width,
                    lane_height - 12,
                };
                clip_blocks.emplace_back(index, block);
                const auto pattern_color =
                    pattern_colors[pattern.instrument];
                const SDL_Color block_color =
                    project.lane_muted(clip.lane)
                        ? SDL_Color{
                              static_cast<std::uint8_t>(pattern_color.r / 2),
                              static_cast<std::uint8_t>(pattern_color.g / 2),
                              static_cast<std::uint8_t>(pattern_color.b / 2),
                              255,
                          }
                        : pattern_color;
                ui.fill(block, block_color);
                std::string block_label = std::to_string(clip.pattern + 1);
                if (width >= 92.0F) {
                    block_label += "  " + pattern.name;
                }
                ui.text(block.x + 8, block.y + 10,
                        block_label,
                        SDL_Color{19, 20, 27, 255});
                if (clip_is_selected(state, index)) {
                    ui.outline(block, SDL_Color{245, 246, 251, 255}, 2);
                }
                if (!ui.hovered(block)) {
                    continue;
                }
                pointer_over_clip = true;
                if (input.right_pressed) {
                    delete_clip = index;
                } else if (input.mouse_pressed) {
                    if (!clip_is_selected(state, index)) {
                        select_only_clip(state, index);
                    } else {
                        state.selected_clip = index;
                    }
                    state.selected_pattern = clip.pattern;
                    if (input.mouse_double_clicked) {
                        state.playlist_resume_step =
                            static_cast<std::size_t>(
                                audible_position /
                                flattened.samples_per_step()) %
                            tiny::song_step_count;
                        state.view = EditorView::piano_roll;
                        switch_to_pattern_audio = true;
                        state.status =
                            "Pattern is playing solo and looping while you edit.";
                        state.status_is_error = false;
                        state.selected_step = 0;
                        SDL_SetWindowTitle(
                            window,
                            "Demo Maker — Pattern Piano Roll");
                    } else if (mouse_playlist_position().has_value()) {
                        begin_drag_clip = index;
                        copy_drag_selection = input.shift_down;
                    }
                }
            }

            if (input.mouse_pressed && ui.hovered(playlist_rect) &&
                !pointer_over_clip &&
                !state.dragging_pattern.has_value()) {
                state.box_selecting_clips = true;
                state.box_selection_start_x =
                    std::clamp(static_cast<float>(input.mouse_x), grid_x,
                               grid_x + grid_width);
                state.box_selection_start_y =
                    std::clamp(static_cast<float>(input.mouse_y), grid_y,
                               grid_y + playlist_rect.height);
                state.box_selection_base =
                    input.shift_down ? state.selected_clips
                                     : std::vector<std::size_t>{};
                if (!input.shift_down) {
                    state.selected_clips.clear();
                    state.selected_clip.reset();
                }
            }

            std::optional<Rect> box_selection;
            if (state.box_selecting_clips) {
                const float current_x =
                    std::clamp(static_cast<float>(input.mouse_x), grid_x,
                               grid_x + grid_width);
                const float current_y =
                    std::clamp(static_cast<float>(input.mouse_y), grid_y,
                               grid_y + playlist_rect.height);
                box_selection = rectangle_between(
                    state.box_selection_start_x,
                    state.box_selection_start_y, current_x, current_y);
                state.selected_clips = state.box_selection_base;
                for (const auto& [index, block] : clip_blocks) {
                    if (!rectangles_intersect(*box_selection, block) ||
                        clip_is_selected(state, index)) {
                        continue;
                    }
                    state.selected_clips.push_back(index);
                }
                if (state.selected_clips.empty()) {
                    state.selected_clip.reset();
                } else {
                    state.selected_clip = state.selected_clips.back();
                    state.selected_pattern =
                        project.clips()[*state.selected_clip].pattern;
                }
                if (input.mouse_released) {
                    state.box_selecting_clips = false;
                    state.box_selection_base.clear();
                    state.status =
                        state.selected_clips.empty()
                            ? "Selection cleared."
                            : std::to_string(state.selected_clips.size()) +
                                  (state.selected_clips.size() == 1
                                       ? " clip selected."
                                       : " clips selected.");
                    state.status_is_error = false;
                }
            }

            if (delete_clip.has_value()) {
                project.remove_clip(*delete_clip);
                state.selected_clip.reset();
                state.selected_clips.clear();
                state.dragging_clips.clear();
                project_changed = true;
            }
            if (begin_drag_clip.has_value() &&
                *begin_drag_clip < project.clips().size()) {
                if (const auto position = mouse_playlist_position()) {
                    if (copy_drag_selection) {
                        std::vector<
                            std::pair<std::size_t, tiny::PlaylistClip>>
                            copies;
                        copies.reserve(state.selected_clips.size());
                        for (const auto selected : state.selected_clips) {
                            if (selected < project.clips().size()) {
                                copies.emplace_back(
                                    selected, project.clips()[selected]);
                            }
                        }

                        state.selected_clips.clear();
                        std::optional<std::size_t> copied_primary;
                        for (const auto& [source_index, copied_clip] :
                             copies) {
                            const auto copied_index =
                                project.add_clip(copied_clip);
                            state.selected_clips.push_back(copied_index);
                            if (source_index == *begin_drag_clip) {
                                copied_primary = copied_index;
                            }
                        }
                        if (!state.selected_clips.empty()) {
                            state.selected_clip =
                                copied_primary.value_or(
                                    state.selected_clips.front());
                            state.selected_pattern =
                                project.clips()[*state.selected_clip].pattern;
                        }
                        state.status =
                            "Copied " +
                            std::to_string(state.selected_clips.size()) +
                            (state.selected_clips.size() == 1
                                 ? " selected clip."
                                 : " selected clips.");
                        state.status_is_error = false;
                        project_changed =
                            project_changed || !copies.empty();
                    }

                    state.dragging_clips.clear();
                    for (const auto selected : state.selected_clips) {
                        if (selected >= project.clips().size()) {
                            continue;
                        }
                        const auto& clip = project.clips()[selected];
                        state.dragging_clips.push_back({
                            .index = selected,
                            .start_step = clip.start_step,
                            .lane = clip.lane,
                        });
                    }
                    state.drag_pointer_step = position->first;
                    state.drag_pointer_lane = position->second;
                }
            }

            if (!state.dragging_clips.empty()) {
                if (const auto position = mouse_playlist_position()) {
                    int minimum_step_delta =
                        -static_cast<int>(tiny::song_step_count);
                    int maximum_step_delta =
                        static_cast<int>(tiny::song_step_count);
                    int minimum_lane_delta =
                        -static_cast<int>(tiny::playlist_lane_count);
                    int maximum_lane_delta =
                        static_cast<int>(tiny::playlist_lane_count);

                    for (const auto& origin : state.dragging_clips) {
                        const auto& clip = project.clips()[origin.index];
                        const auto length =
                            project.patterns()[clip.pattern].length_steps;
                        minimum_step_delta =
                            std::max(minimum_step_delta,
                                     -static_cast<int>(origin.start_step));
                        maximum_step_delta = std::min(
                            maximum_step_delta,
                            static_cast<int>(tiny::song_step_count - length) -
                                static_cast<int>(origin.start_step));
                        minimum_lane_delta =
                            std::max(minimum_lane_delta,
                                     -static_cast<int>(origin.lane));
                        maximum_lane_delta = std::min(
                            maximum_lane_delta,
                            static_cast<int>(tiny::playlist_lane_count - 1) -
                                static_cast<int>(origin.lane));
                    }

                    const int requested_step_delta =
                        static_cast<int>(position->first) -
                        static_cast<int>(state.drag_pointer_step);
                    const int requested_lane_delta =
                        static_cast<int>(position->second) -
                        static_cast<int>(state.drag_pointer_lane);
                    const int step_delta =
                        std::clamp(requested_step_delta, minimum_step_delta,
                                   maximum_step_delta);
                    const int lane_delta =
                        std::clamp(requested_lane_delta, minimum_lane_delta,
                                   maximum_lane_delta);

                    for (const auto& origin : state.dragging_clips) {
                        auto& clip = project.clips()[origin.index];
                        const auto next_start = static_cast<std::uint16_t>(
                            static_cast<int>(origin.start_step) + step_delta);
                        const auto next_lane = static_cast<std::uint8_t>(
                            static_cast<int>(origin.lane) + lane_delta);
                        if (clip.start_step != next_start ||
                            clip.lane != next_lane) {
                            clip.start_step = next_start;
                            clip.lane = next_lane;
                            project_changed = true;
                        }
                    }
                }
                if (input.mouse_released) {
                    state.dragging_clips.clear();
                }
            }

            if (state.dragging_pattern.has_value()) {
                if (const auto position = mouse_playlist_position()) {
                    const auto& pattern =
                        project.patterns()[*state.dragging_pattern];
                    const float ghost_x =
                        grid_x +
                        static_cast<float>(position->first) * step_width;
                    const float ghost_y =
                        grid_y + static_cast<float>(position->second) *
                                     lane_height +
                        5;
                    ui.fill({ghost_x, ghost_y,
                             static_cast<float>(pattern.length_steps) *
                                     step_width -
                                 3,
                             lane_height - 12},
                            SDL_Color{pattern_colors[pattern.instrument].r,
                                      pattern_colors[pattern.instrument].g,
                                      pattern_colors[pattern.instrument].b,
                                      150});
                    if (input.mouse_released) {
                        const auto added_clip = project.add_clip({
                            .start_step =
                                static_cast<std::uint16_t>(position->first),
                            .lane = static_cast<std::uint8_t>(position->second),
                            .pattern = static_cast<std::uint8_t>(
                                *state.dragging_pattern),
                        });
                        select_only_clip(state, added_clip);
                        project_changed = true;
                    }
                }
                if (input.mouse_released) {
                    state.dragging_pattern.reset();
                }
            }

            if (box_selection.has_value()) {
                ui.fill(*box_selection, SDL_Color{112, 137, 255, 38});
                ui.outline(*box_selection,
                           SDL_Color{164, 181, 255, 230}, 2);
            }

            // Keep the transport marker above pattern blocks so its timing
            // remains readable across every playlist lane.
            if (state.playing && playhead_step >= 0.0 &&
                playhead_step < static_cast<double>(visible_steps)) {
                const float x =
                    grid_x + static_cast<float>(playhead_step) * step_width;
                ui.fill({x, grid_y, 3, playlist_rect.height}, playhead);
            }
        } else {
            if (ui.button({24, 82, 150, 36}, "< BACK TO PLAYLIST")) {
                state.view = EditorView::playlist;
                switch_to_playlist_audio = true;
                state.status =
                    "Playlist playback restored at the previous position.";
                state.status_is_error = false;
                SDL_SetWindowTitle(window, "Demo Maker — Playlist");
            }
            ui.text(194, 91,
                    "PATTERN " + std::to_string(state.selected_pattern + 1) +
                        "  " + selected_pattern.name,
                    foreground);
            ui.text(editor_right - 126, 91, "SOLO LOOP", playhead);

            ui.text(24, 139, "INSTRUMENT", muted);
            for (std::size_t instrument = 0; instrument < tiny::track_count;
                 ++instrument) {
                if (ui.button(
                        {130.0F + static_cast<float>(instrument) * 92.0F, 126,
                         82, 34},
                        track_names[instrument],
                        selected_pattern.instrument == instrument)) {
                    selected_pattern.instrument =
                        static_cast<std::uint8_t>(instrument);
                    state.selected_note =
                        (state.view_octaves[instrument] + 1) * 12;
                    project_changed = true;
                }
            }

            auto& view_octave =
                state.view_octaves[selected_pattern.instrument];
            ui.text(520, 139,
                    "INPUT OCTAVE " + std::to_string(view_octave), muted);
            if (ui.button({650, 126, 34, 34}, "-")) {
                view_octave = std::max(0, view_octave - 1);
            }
            if (ui.button({690, 126, 34, 34}, "+")) {
                view_octave = std::min(8, view_octave + 1);
            }
            ui.text(746, 139,
                    "ZOOM " +
                        std::to_string(static_cast<int>(std::lround(
                            state.piano_horizontal_zoom * 100.0F))) +
                        "%",
                    muted);

            const float grid_x = 112.0F;
            const float grid_y = 195.0F;
            const float grid_width =
                std::max(460.0F, editor_right - grid_x - 38);
            constexpr float grid_height = 472.0F;
            const Rect piano_roll{grid_x, grid_y, grid_width, grid_height};

            // Wheel navigation follows the FL-style split: wheel scrolls the
            // pitch view, while Ctrl+wheel zooms around the mouse position.
            if (input.wheel_y != 0 && ui.hovered(piano_roll)) {
                if (input.control_down) {
                    const float horizontal_anchor =
                        state.piano_scroll_step +
                        (static_cast<float>(input.mouse_x) - grid_x) /
                            (grid_width /
                             static_cast<float>(
                                 selected_pattern.length_steps) *
                             state.piano_horizontal_zoom);
                    const float old_row_height =
                        21.0F * state.piano_vertical_zoom;
                    const float pitch_anchor =
                        state.piano_top_notes[selected_pattern.instrument] -
                        (static_cast<float>(input.mouse_y) - grid_y) /
                            old_row_height;
                    const float zoom_factor =
                        std::pow(1.16F, static_cast<float>(input.wheel_y));

                    state.piano_horizontal_zoom = std::clamp(
                        state.piano_horizontal_zoom * zoom_factor, 1.0F, 8.0F);
                    state.piano_vertical_zoom = std::clamp(
                        state.piano_vertical_zoom * zoom_factor, 0.65F, 3.0F);

                    const float next_cell_width =
                        grid_width /
                        static_cast<float>(selected_pattern.length_steps) *
                        state.piano_horizontal_zoom;
                    state.piano_scroll_step =
                        horizontal_anchor -
                        (static_cast<float>(input.mouse_x) - grid_x) /
                            next_cell_width;
                    const float next_row_height =
                        21.0F * state.piano_vertical_zoom;
                    state.piano_top_notes[selected_pattern.instrument] =
                        pitch_anchor +
                        (static_cast<float>(input.mouse_y) - grid_y) /
                            next_row_height;
                } else {
                    state.piano_top_notes[selected_pattern.instrument] +=
                        static_cast<float>(input.wheel_y * 3);
                }
            }

            const float cell_width =
                grid_width /
                static_cast<float>(selected_pattern.length_steps) *
                state.piano_horizontal_zoom;
            const float visible_steps = grid_width / cell_width;
            const float maximum_step_scroll =
                std::max(0.0F, static_cast<float>(
                                   selected_pattern.length_steps) -
                                   visible_steps);
            state.piano_scroll_step =
                std::clamp(state.piano_scroll_step, 0.0F, maximum_step_scroll);

            const float row_height = 21.0F * state.piano_vertical_zoom;
            const float visible_pitch_rows = grid_height / row_height;
            auto& top_note =
                state.piano_top_notes[selected_pattern.instrument];
            top_note = std::clamp(top_note, visible_pitch_rows - 1.0F, 127.0F);
            const int rendered_rows =
                static_cast<int>(std::ceil(visible_pitch_rows));
            const int highest_note = static_cast<int>(std::floor(top_note));

            for (std::size_t step = 0;
                 step < selected_pattern.length_steps; ++step) {
                const float step_x =
                    grid_x +
                    (static_cast<float>(step) - state.piano_scroll_step) *
                        cell_width;
                if (step % 4 == 0 && step_x >= grid_x - cell_width &&
                    step_x < grid_x + grid_width) {
                    ui.text(step_x + 5,
                            grid_y - 27, std::to_string(step + 1), foreground);
                }
            }

            for (int row = 0; row < rendered_rows; ++row) {
                const int pitch = highest_note - row;
                if (pitch < 0) {
                    break;
                }
                const int pitch_class = pitch % 12;
                const bool black_key =
                    pitch_class == 1 || pitch_class == 3 || pitch_class == 6 ||
                    pitch_class == 8 || pitch_class == 10;
                const float row_y =
                    grid_y + static_cast<float>(row) * row_height;
                const float visible_row_height =
                    std::min(row_height, grid_y + grid_height - row_y);
                ui.fill({24, row_y, grid_x - 30, visible_row_height - 1},
                        black_key ? SDL_Color{35, 37, 49, 255}
                                  : SDL_Color{202, 205, 216, 255});
                ui.text(34, row_y + 1, note_name(pitch),
                        black_key ? foreground
                                  : SDL_Color{35, 37, 49, 255});

                for (std::size_t step = 0;
                     step < selected_pattern.length_steps; ++step) {
                    const float raw_x =
                        grid_x +
                        (static_cast<float>(step) -
                         state.piano_scroll_step) *
                            cell_width;
                    const float visible_left = std::max(raw_x, grid_x);
                    const float visible_right =
                        std::min(raw_x + cell_width, grid_x + grid_width);
                    if (visible_right <= visible_left) {
                        continue;
                    }
                    const Rect cell{
                        visible_left,
                        row_y,
                        visible_right - visible_left,
                        visible_row_height,
                    };
                    SDL_Color cell_color =
                        black_key ? SDL_Color{25, 28, 40, 255} : panel_alt;
                    if ((step / 4) % 2 == 1) {
                        cell_color =
                            black_key ? SDL_Color{29, 31, 45, 255}
                                      : SDL_Color{37, 40, 56, 255};
                    }
                    ui.fill(cell, cell_color);
                    if (state.playing &&
                        sync.step % selected_pattern.length_steps == step) {
                        ui.fill(cell, SDL_Color{47, 171, 145, 42});
                    }
                    if (step % 4 == 0 && raw_x >= grid_x) {
                        ui.fill({raw_x, cell.y, 2, cell.height},
                                SDL_Color{78, 81, 105, 255});
                    }
                    ui.fill({cell.x, cell.y + cell.height - 1, cell.width, 1},
                            pitch_class == 0
                                ? SDL_Color{83, 87, 112, 255}
                                : SDL_Color{48, 52, 70, 255});

                    auto& note = selected_pattern.notes[step];
                    if (input.mouse_down && ui.hovered(cell)) {
                        if (note.note != pitch) {
                            note = {
                                .note = static_cast<std::int8_t>(pitch),
                                .instrument = selected_pattern.instrument,
                                .gate_steps =
                                    default_gate(selected_pattern.instrument),
                            };
                            audition_note = pitch;
                            project_changed = true;
                        }
                        state.selected_step = step;
                        state.selected_note = pitch;
                    }
                    if (input.right_down && ui.hovered(cell)) {
                        if (note.note >= 0) {
                            note = {};
                            project_changed = true;
                        }
                        state.selected_step = step;
                        state.selected_note = pitch;
                    }
                }

                for (std::size_t step = 0;
                     step < selected_pattern.length_steps; ++step) {
                    const auto& note = selected_pattern.notes[step];
                    if (note.note != pitch) {
                        continue;
                    }
                    const float note_x =
                        grid_x +
                        (static_cast<float>(step) -
                         state.piano_scroll_step) *
                            cell_width;
                    const float clipped_left = std::max(note_x + 2, grid_x);
                    const float clipped_right = std::min(
                        note_x +
                            cell_width * static_cast<float>(note.gate_steps) -
                            1,
                        grid_x + grid_width);
                    if (clipped_right <= clipped_left) {
                        continue;
                    }
                    const Rect note_block{
                        clipped_left,
                        row_y + 2,
                        clipped_right - clipped_left,
                        std::max(2.0F, visible_row_height - 4),
                    };
                    ui.fill(note_block,
                            pattern_colors[selected_pattern.instrument]);
                    if (state.selected_step == step &&
                        state.selected_note == pitch) {
                        ui.outline(note_block,
                                   SDL_Color{245, 242, 255, 255}, 2);
                    }
                }
            }

            const Rect horizontal_scrollbar{
                grid_x,
                grid_width,
                grid_y + grid_height + 5,
                13,
            };
            const Rect vertical_scrollbar{
                grid_x + grid_width + 5,
                grid_y,
                13,
                grid_height,
            };
            ui.fill(horizontal_scrollbar, SDL_Color{35, 38, 52, 255});
            ui.fill(vertical_scrollbar, SDL_Color{35, 38, 52, 255});

            const float horizontal_handle_width = std::max(
                32.0F, horizontal_scrollbar.width /
                           state.piano_horizontal_zoom);
            const float horizontal_travel =
                horizontal_scrollbar.width - horizontal_handle_width;
            const float horizontal_fraction =
                maximum_step_scroll > 0.0F
                    ? state.piano_scroll_step / maximum_step_scroll
                    : 0.0F;
            Rect horizontal_handle{
                horizontal_scrollbar.x +
                    horizontal_fraction * horizontal_travel,
                horizontal_scrollbar.y,
                horizontal_handle_width,
                horizontal_scrollbar.height,
            };
            ui.fill(horizontal_handle, SDL_Color{102, 108, 136, 255});

            const float maximum_pitch_scroll =
                std::max(0.0F, 128.0F - visible_pitch_rows);
            const float vertical_handle_height = std::max(
                28.0F, vertical_scrollbar.height *
                           std::min(1.0F, visible_pitch_rows / 128.0F));
            const float vertical_travel =
                vertical_scrollbar.height - vertical_handle_height;
            const float vertical_fraction =
                maximum_pitch_scroll > 0.0F
                    ? (127.0F - top_note) / maximum_pitch_scroll
                    : 0.0F;
            Rect vertical_handle{
                vertical_scrollbar.x,
                vertical_scrollbar.y + vertical_fraction * vertical_travel,
                vertical_scrollbar.width,
                vertical_handle_height,
            };
            ui.fill(vertical_handle, SDL_Color{102, 108, 136, 255});

            if (input.mouse_pressed && ui.hovered(horizontal_scrollbar)) {
                state.dragging_horizontal_scrollbar = true;
                state.dragging_vertical_scrollbar = false;
                state.scrollbar_drag_offset =
                    ui.hovered(horizontal_handle)
                        ? static_cast<float>(input.mouse_x) -
                              horizontal_handle.x
                        : horizontal_handle.width * 0.5F;
            }
            if (state.dragging_horizontal_scrollbar && input.mouse_down) {
                const float handle_x = std::clamp(
                    static_cast<float>(input.mouse_x) -
                        state.scrollbar_drag_offset,
                    horizontal_scrollbar.x,
                    horizontal_scrollbar.x + horizontal_travel);
                const float fraction =
                    horizontal_travel > 0.0F
                        ? (handle_x - horizontal_scrollbar.x) /
                              horizontal_travel
                        : 0.0F;
                state.piano_scroll_step = fraction * maximum_step_scroll;
            }

            if (input.mouse_pressed && ui.hovered(vertical_scrollbar)) {
                state.dragging_vertical_scrollbar = true;
                state.dragging_horizontal_scrollbar = false;
                state.scrollbar_drag_offset =
                    ui.hovered(vertical_handle)
                        ? static_cast<float>(input.mouse_y) - vertical_handle.y
                        : vertical_handle.height * 0.5F;
            }
            if (state.dragging_vertical_scrollbar && input.mouse_down) {
                const float handle_y = std::clamp(
                    static_cast<float>(input.mouse_y) -
                        state.scrollbar_drag_offset,
                    vertical_scrollbar.y,
                    vertical_scrollbar.y + vertical_travel);
                const float fraction =
                    vertical_travel > 0.0F
                        ? (handle_y - vertical_scrollbar.y) / vertical_travel
                        : 0.0F;
                top_note =
                    127.0F - fraction * maximum_pitch_scroll;
            }
            if (input.mouse_released) {
                state.dragging_horizontal_scrollbar = false;
                state.dragging_vertical_scrollbar = false;
            }

            const float properties_y =
                horizontal_scrollbar.y + horizontal_scrollbar.height + 8;
            auto& selected_note =
                selected_pattern.notes[state.selected_step];
            const bool has_selected_note = selected_note.note >= 0;
            const Rect note_properties{
                24,
                properties_y,
                editor_right - 44,
                78,
            };
            ui.fill(note_properties, panel);
            ui.outline(note_properties, SDL_Color{55, 59, 78, 255});

            ui.text(42, properties_y + 11, "SELECTED NOTE", muted);
            ui.text(42, properties_y + 39,
                    has_selected_note ? note_name(selected_note.note) : "NONE",
                    has_selected_note ? foreground : muted);

            ui.text(180, properties_y + 11, "START POSITION", muted);
            ui.text(180, properties_y + 39,
                    "STEP " + std::to_string(state.selected_step + 1),
                    foreground);

            if (has_selected_note) {
                float length_steps = static_cast<float>(
                    std::max<std::uint8_t>(1, selected_note.gate_steps));
                if (ui.slider(101,
                              {330, properties_y + 9,
                               std::max(220.0F, editor_right - 374.0F), 40},
                              "NOTE LENGTH (STEPS)", length_steps, 1.0F, 8.0F,
                              0)) {
                    selected_note.gate_steps =
                        static_cast<std::uint8_t>(std::lround(length_steps));
                    project_changed = true;
                }
            } else {
                ui.text(330, properties_y + 11, "NOTE LENGTH", muted);
                ui.text(330, properties_y + 39,
                        "Select or paint a note to edit its length", muted);
            }
        }

        // The selected pattern owns the instrument shown in the common panel.
        project_changed |= draw_instrument_panel(
            ui, project, project.patterns()[state.selected_pattern], panel_x,
            panel_width);

        const float footer_y =
            std::max(820.0F, static_cast<float>(window_height) - 62.0F);
        ui.text(24, footer_y,
                state.view == EditorView::playlist
                    ? "Box-drag: select   Shift+box: add   drag: move   "
                      "Shift+drag: copy selection   double-click: piano roll"
                    : "Paint: left-drag   erase: right-drag   wheel: pitch   "
                      "Ctrl+wheel: zoom   Esc: playlist",
                muted);
        ui.text(24, footer_y + 27, state.status,
                state.status_is_error ? warning : playhead);

        if (toggle_playback) {
            state.playing = !state.playing;
            SDL_PauseAudioDevice(audio_device, state.playing ? 0 : 1);
        }
        if (stop_playback) {
            state.playing = false;
            SDL_PauseAudioDevice(audio_device, 1);
            SDL_LockAudioDevice(audio_device);
            audio.synth.reset();
            audio.generated_samples.store(0, std::memory_order_release);
            audio.rendered_samples.store(0, std::memory_order_release);
            SDL_UnlockAudioDevice(audio_device);
        }
        if (request_load && load_project(project, project_path, state)) {
            state.selected_pattern = 0;
            state.selected_clip.reset();
            state.selected_clips.clear();
            state.box_selecting_clips = false;
            state.dragging_clips.clear();
            state.view = EditorView::playlist;
            switch_to_pattern_audio = false;
            switch_to_playlist_audio = false;
            project_changed = true;
            reset_transport = true;
            SDL_SetWindowTitle(window, "Demo Maker — Playlist");
        }
        if (request_save) {
            save_project(project, project_path, state);
        }
        if (request_export) {
            export_project(project, project_path, state);
        }
        if (switch_to_pattern_audio) {
            apply_pattern_audio(audio_device, audio, project,
                                state.selected_pattern, true);
        } else if (switch_to_playlist_audio) {
            apply_playlist_audio(audio_device, audio, project, false,
                                 state.playlist_resume_step);
        } else if (project_changed) {
            if (state.view == EditorView::piano_roll) {
                apply_pattern_audio(audio_device, audio, project,
                                    state.selected_pattern, reset_transport);
            } else {
                apply_playlist_audio(audio_device, audio, project,
                                     reset_transport);
            }
        }
        if (audition_note.has_value()) {
            audition(audio_device, audio, *audition_note,
                     project.patterns()[state.selected_pattern].instrument);
        }

        SDL_RenderPresent(renderer);
    }

    SDL_CloseAudioDevice(audio_device);
    ui.shutdown();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
