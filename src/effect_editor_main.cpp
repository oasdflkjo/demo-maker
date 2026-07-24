#include "editor_ui.hpp"
#include "gl_renderer.hpp"
#include "tiny/effect.hpp"
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
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace {

using tiny::editor::Rect;

constexpr SDL_Color background{18, 20, 29, 255};
constexpr SDL_Color panel{27, 30, 43, 255};
constexpr SDL_Color foreground{226, 229, 241, 255};
constexpr SDL_Color muted{123, 129, 154, 255};
constexpr SDL_Color healthy{47, 171, 145, 255};
constexpr SDL_Color warning{242, 184, 92, 255};

struct AudioState {
    AudioState(tiny::Song song, tiny::StepRange loop)
        : synth(std::move(song)) {
        synth.set_loop_steps(loop.start, loop.length);
    }

    tiny::SynthEngine synth;
    std::atomic<std::uint64_t> generated_samples{0};
    std::atomic<std::uint64_t> rendered_samples{0};
};

struct Status {
    std::string message{"Shader parameters are live."};
    bool error{false};
};

enum class ControlsTab {
    shader,
    text,
};

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

int decimals_for_step(float step) {
    if (step >= 1.0F) {
        return 0;
    }
    if (step >= 0.1F) {
        return 1;
    }
    if (step >= 0.01F) {
        return 2;
    }
    return 3;
}

std::filesystem::path find_default_project(std::string_view executable) {
    std::error_code error;
    const auto current = std::filesystem::current_path(error);
    if (!error) {
        const auto local = current / "song.tds";
        if (std::filesystem::exists(local)) {
            return local;
        }
    }

    const auto executable_path =
        std::filesystem::absolute(std::filesystem::path(executable), error);
    if (!error) {
        const auto executable_directory = executable_path.parent_path();
        const std::array candidates{
            executable_directory / "song.tds",
            executable_directory.parent_path() / "song.tds",
        };
        for (const auto& candidate : candidates) {
            if (std::filesystem::exists(candidate)) {
                return candidate;
            }
        }
    }
    return "song.tds";
}

bool load_music(const std::filesystem::path& path, tiny::Song& song,
                tiny::StepRange& loop, std::string& error) {
    if (path.extension() == ".bin") {
        if (!tiny::load_song_binary(path, song, error)) {
            return false;
        }
        loop = song.populated_range();
        return true;
    }

    tiny::StudioProject studio;
    std::string studio_error;
    if (tiny::load_studio_project(path, studio, studio_error)) {
        loop = studio.populated_range();
        song = studio.compile_song();
        error.clear();
        return true;
    }

    std::string legacy_error;
    if (tiny::load_song_project(path, song, legacy_error)) {
        loop = song.populated_range();
        error.clear();
        return true;
    }

    error = studio_error;
    if (!legacy_error.empty()) {
        error += " (" + legacy_error + ")";
    }
    return false;
}

void save_preset(const tiny::EffectSettings& settings,
                 const std::filesystem::path& preset_path, Status& status) {
    std::string error;
    if (settings.save_preset(preset_path, error)) {
        status = {"Saved " + preset_path.string(), false};
    } else {
        status = {error, true};
    }
}

void load_preset(tiny::EffectSettings& settings,
                 const std::filesystem::path& preset_path, Status& status) {
    std::string error;
    if (settings.load_preset(preset_path, error)) {
        status = {"Loaded " + preset_path.string(), false};
    } else {
        status = {error, true};
    }
}

void print_help(std::string_view executable) {
    std::cout << "Usage: " << executable
              << " [--preset FILE] [--shader FILE] [--project FILE]"
                 " [--song FILE]"
                 " [--font FILE]\n";
}

} // namespace

int main(int argc, char** argv) {
    std::filesystem::path shader_path = TINY_DEMO_SHADER_PATH;
    std::filesystem::path preset_path = "starfield.fxp";
    std::filesystem::path font_path = find_font();
    std::filesystem::path song_path = find_default_project(argv[0]);
    bool song_path_was_explicit = false;

    for (int index = 1; index < argc; ++index) {
        const std::string_view argument = argv[index];
        if (argument == "--help" || argument == "-h") {
            print_help(argv[0]);
            return 0;
        }
        if (argument == "--preset" && index + 1 < argc) {
            preset_path = argv[++index];
            continue;
        }
        if (argument == "--shader" && index + 1 < argc) {
            shader_path = argv[++index];
            continue;
        }
        if ((argument == "--project" || argument == "--song") &&
            index + 1 < argc) {
            song_path = argv[++index];
            song_path_was_explicit = true;
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

    tiny::EffectSettings settings;
    std::string error;
    if (!settings.load_schema(shader_path, false, error)) {
        std::cerr << error << '\n';
        return 1;
    }

    Status status;
    if (std::filesystem::exists(preset_path)) {
        load_preset(settings, preset_path, status);
    }

    tiny::Song song = tiny::Song::make_demo_song();
    tiny::StepRange song_loop = song.populated_range();
    if (std::filesystem::exists(song_path)) {
        if (!load_music(song_path, song, song_loop, error)) {
            std::cerr << error << '\n';
            return 1;
        }
        status = {"Music: " + song_path.string(), false};
        std::cout << "Loaded music project: " << song_path << std::endl;
    } else if (song_path_was_explicit) {
        std::cerr << "Could not open music project: " << song_path << '\n';
        return 1;
    } else {
        status = {"No song.tds found; using the built-in demo song.", false};
        std::cout << status.message << std::endl;
    }

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0) {
        std::cerr << "SDL initialization failed: " << SDL_GetError() << '\n';
        return 1;
    }

    SDL_Window* controls_window = SDL_CreateWindow(
        "Demo Maker — Effect Controls", 60, 80, 520, 980,
        SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (controls_window == nullptr) {
        std::cerr << "Control window creation failed: " << SDL_GetError()
                  << '\n';
        SDL_Quit();
        return 1;
    }
    SDL_SetWindowMinimumSize(controls_window, 520, 920);

    SDL_Renderer* controls_renderer =
        SDL_CreateRenderer(controls_window, -1, SDL_RENDERER_ACCELERATED);
    if (controls_renderer == nullptr) {
        controls_renderer = SDL_CreateRenderer(controls_window, -1, 0);
    }
    if (controls_renderer == nullptr) {
        std::cerr << "Control renderer creation failed: " << SDL_GetError()
                  << '\n';
        SDL_DestroyWindow(controls_window);
        SDL_Quit();
        return 1;
    }

    tiny::editor::Ui ui;
    if (!ui.initialize(controls_renderer, font_path, error)) {
        std::cerr << error << '\n';
        SDL_DestroyRenderer(controls_renderer);
        SDL_DestroyWindow(controls_window);
        SDL_Quit();
        return 1;
    }

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,
                        SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

    SDL_Window* preview_window = SDL_CreateWindow(
        "Demo Maker — Live Preview", 610, 180, 960, 540,
        SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (preview_window == nullptr) {
        std::cerr << "Preview window creation failed: " << SDL_GetError()
                  << '\n';
        ui.shutdown();
        SDL_DestroyRenderer(controls_renderer);
        SDL_DestroyWindow(controls_window);
        SDL_Quit();
        return 1;
    }

    SDL_GLContext preview_context = SDL_GL_CreateContext(preview_window);
    if (preview_context == nullptr) {
        std::cerr << "Preview context creation failed: " << SDL_GetError()
                  << '\n';
        SDL_DestroyWindow(preview_window);
        ui.shutdown();
        SDL_DestroyRenderer(controls_renderer);
        SDL_DestroyWindow(controls_window);
        SDL_Quit();
        return 1;
    }
    SDL_GL_MakeCurrent(preview_window, preview_context);
    SDL_GL_SetSwapInterval(1);

    tiny::GlRenderer preview;
    if (!preview.initialize(shader_path)) {
        std::cerr << preview.error() << '\n';
    }

    AudioState audio(song, song_loop);
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
        preview.shutdown();
        SDL_GL_DeleteContext(preview_context);
        SDL_DestroyWindow(preview_window);
        ui.shutdown();
        SDL_DestroyRenderer(controls_renderer);
        SDL_DestroyWindow(controls_window);
        SDL_Quit();
        return 1;
    }
    SDL_PauseAudioDevice(audio_device, 0);

    const auto controls_id = SDL_GetWindowID(controls_window);
    const auto preview_id = SDL_GetWindowID(preview_window);
    bool running = true;
    bool playing = true;
    bool preview_visible = true;
    ControlsTab controls_tab = ControlsTab::shader;
    std::size_t selected_text = 0;
    bool editing_text = false;
    std::uint32_t last_reload_check = 0;

    while (running) {
        tiny::editor::Input input;
        if (SDL_GetMouseFocus() == controls_window) {
            const auto buttons =
                SDL_GetMouseState(&input.mouse_x, &input.mouse_y);
            input.mouse_down = (buttons & SDL_BUTTON_LMASK) != 0;
        } else {
            input.mouse_x = -10'000;
            input.mouse_y = -10'000;
        }

        bool request_save = false;
        bool request_load = false;
        bool request_reset = false;
        bool request_reload = false;
        bool toggle_playback = false;

        SDL_Event event{};
        while (SDL_PollEvent(&event) != 0) {
            if (event.type == SDL_QUIT) {
                running = false;
            } else if (event.type == SDL_WINDOWEVENT &&
                       event.window.event == SDL_WINDOWEVENT_CLOSE) {
                if (event.window.windowID == controls_id) {
                    running = false;
                } else if (event.window.windowID == preview_id) {
                    SDL_HideWindow(preview_window);
                    preview_visible = false;
                }
            } else if (event.type == SDL_MOUSEMOTION &&
                       event.motion.windowID == controls_id) {
                input.mouse_x = event.motion.x;
                input.mouse_y = event.motion.y;
            } else if (event.type == SDL_MOUSEBUTTONDOWN &&
                       event.button.windowID == controls_id) {
                input.mouse_x = event.button.x;
                input.mouse_y = event.button.y;
                if (event.button.button == SDL_BUTTON_LEFT) {
                    input.mouse_pressed = true;
                    input.mouse_down = true;
                }
            } else if (event.type == SDL_MOUSEBUTTONUP &&
                       event.button.windowID == controls_id &&
                       event.button.button == SDL_BUTTON_LEFT) {
                input.mouse_released = true;
                input.mouse_down = false;
            } else if (event.type == SDL_TEXTINPUT &&
                       event.text.windowID == controls_id && editing_text &&
                       selected_text < settings.texts().size()) {
                auto& text = settings.texts()[selected_text].text;
                for (const char character :
                     std::string_view(event.text.text)) {
                    const auto byte =
                        static_cast<unsigned char>(character);
                    if (byte >= 32 && byte <= 126 && text.size() < 128) {
                        text.push_back(character);
                    }
                }
            } else if (event.type == SDL_KEYDOWN &&
                       event.key.windowID == controls_id) {
                if (editing_text) {
                    if (event.key.keysym.sym == SDLK_BACKSPACE &&
                        selected_text < settings.texts().size() &&
                        !settings.texts()[selected_text].text.empty()) {
                        settings.texts()[selected_text].text.pop_back();
                    } else if (event.key.keysym.sym == SDLK_RETURN ||
                               event.key.keysym.sym == SDLK_KP_ENTER ||
                               event.key.keysym.sym == SDLK_ESCAPE) {
                        editing_text = false;
                        SDL_StopTextInput();
                    }
                    continue;
                }
                const auto modifiers =
                    static_cast<SDL_Keymod>(event.key.keysym.mod);
                const bool control = (modifiers & KMOD_CTRL) != 0;
                if (event.key.keysym.sym == SDLK_ESCAPE) {
                    running = false;
                } else if (event.key.keysym.sym == SDLK_SPACE &&
                           event.key.repeat == 0) {
                    toggle_playback = true;
                } else if (control && event.key.keysym.sym == SDLK_s) {
                    request_save = true;
                } else if (control && event.key.keysym.sym == SDLK_o) {
                    request_load = true;
                } else if (control && event.key.keysym.sym == SDLK_r) {
                    request_reload = true;
                }
            }
        }

        int controls_width = 0;
        int controls_height = 0;
        SDL_GetWindowSize(controls_window, &controls_width, &controls_height);
        SDL_RenderSetLogicalSize(controls_renderer, controls_width,
                                 controls_height);
        ui.begin_frame(input);

        SDL_SetRenderDrawColor(controls_renderer, background.r, background.g,
                               background.b, background.a);
        SDL_RenderClear(controls_renderer);
        ui.fill({0, 0, static_cast<float>(controls_width), 82}, panel);
        ui.text(22, 16, "DEMO MAKER", foreground);
        ui.text(22, 42, "LIVE EFFECT PARAMETERS", muted);

        if (ui.button({245, 20, 78, 38}, playing ? "PAUSE" : "PLAY",
                      playing)) {
            toggle_playback = true;
        }
        if (ui.button({333, 20, 72, 38}, "SAVE")) {
            request_save = true;
        }
        if (ui.button({415, 20, 76, 38}, "LOAD")) {
            request_load = true;
        }

        if (ui.button({22, 98, 92, 36}, "SHADER",
                      controls_tab == ControlsTab::shader)) {
            controls_tab = ControlsTab::shader;
            editing_text = false;
            SDL_StopTextInput();
        }
        if (ui.button({124, 98, 92, 36}, "TEXT",
                      controls_tab == ControlsTab::text)) {
            controls_tab = ControlsTab::text;
        }
        if (ui.button({226, 98, 130, 36},
                      preview_visible ? "HIDE PREVIEW" : "SHOW PREVIEW",
                      preview_visible)) {
            preview_visible = !preview_visible;
            if (preview_visible) {
                SDL_ShowWindow(preview_window);
                SDL_RaiseWindow(preview_window);
            } else {
                SDL_HideWindow(preview_window);
            }
        }

        const float parameter_width =
            static_cast<float>(controls_width) - 44.0F;
        float y = 156.0F;
        if (controls_tab == ControlsTab::shader) {
            if (ui.button({22, y, 92, 36}, "DEFAULTS")) {
                request_reset = true;
            }
            if (ui.button({124, y, 92, 36}, "RELOAD")) {
                request_reload = true;
            }
            y += 52.0F;

            int slider_id = 1000;
            for (auto& parameter : settings.parameters()) {
                if (ui.slider(slider_id++, {22, y, parameter_width, 40},
                              parameter.label, parameter.value,
                              parameter.minimum, parameter.maximum,
                              decimals_for_step(parameter.step))) {
                    const float steps =
                        std::round((parameter.value - parameter.minimum) /
                                   parameter.step);
                    parameter.value = std::clamp(
                        parameter.minimum + steps * parameter.step,
                        parameter.minimum, parameter.maximum);
                }
                y += 55.0F;
            }
        } else {
            if (ui.button({22, y, 76, 36}, "ADD")) {
                selected_text = settings.add_text();
                editing_text = false;
                SDL_StopTextInput();
                status = {"Added a pixel text overlay.", false};
            }
            if (!settings.texts().empty()) {
                if (ui.button({108, y, 34, 36}, "<")) {
                    selected_text =
                        (selected_text + settings.texts().size() - 1) %
                        settings.texts().size();
                    editing_text = false;
                    SDL_StopTextInput();
                }
                ui.text(154, y + 9,
                        std::to_string(selected_text + 1) + " / " +
                            std::to_string(settings.texts().size()),
                        foreground);
                if (ui.button({226, y, 34, 36}, ">")) {
                    selected_text =
                        (selected_text + 1) % settings.texts().size();
                    editing_text = false;
                    SDL_StopTextInput();
                }
                if (ui.button({270, y, 92, 36}, "DELETE")) {
                    settings.remove_text(selected_text);
                    if (settings.texts().empty()) {
                        selected_text = 0;
                    } else {
                        selected_text =
                            std::min(selected_text,
                                     settings.texts().size() - 1);
                    }
                    editing_text = false;
                    SDL_StopTextInput();
                    status = {"Deleted the text overlay.", false};
                }
            }
            y += 52.0F;

            if (settings.texts().empty()) {
                ui.text(22, y + 12,
                        "ADD A TEXT LAYER TO PLACE PIXEL TYPE IN THE DEMO.",
                        muted);
                y += 52.0F;
            } else {
                selected_text =
                    std::min(selected_text, settings.texts().size() - 1);
                auto& text = settings.texts()[selected_text];
                std::string field_text = text.text;
                if (field_text.size() > 42) {
                    field_text = field_text.substr(0, 39) + "...";
                }
                if (editing_text) {
                    field_text += "_";
                }
                if (ui.button({22, y, parameter_width, 40},
                              "TEXT  " + field_text, editing_text)) {
                    editing_text = true;
                    SDL_StartTextInput();
                }
                y += 52.0F;
                ui.checkbox({22, y, 176, 36}, "VISIBLE",
                            text.enabled);
                if (ui.button({208, y, parameter_width - 186.0F, 36},
                              "AUTO CENTER")) {
                    int preview_width = 0;
                    int preview_height = 0;
                    SDL_GL_GetDrawableSize(preview_window, &preview_width,
                                           &preview_height);
                    tiny::center_text_overlay(
                        text, static_cast<float>(preview_width),
                        static_cast<float>(preview_height));
                    status = {"Centered text in the preview.", false};
                }
                y += 48.0F;

                int slider_id = 2000;
                ui.slider(slider_id++, {22, y, parameter_width, 40},
                          "POSITION X", text.x, 0.0F, 1.0F, 2);
                y += 55.0F;
                ui.slider(slider_id++, {22, y, parameter_width, 40},
                          "POSITION Y", text.y, 0.0F, 1.0F, 2);
                y += 55.0F;
                if (ui.slider(slider_id++, {22, y, parameter_width, 40},
                              "PIXEL SIZE", text.scale, 1.0F, 16.0F, 0)) {
                    text.scale = std::round(text.scale);
                }
                y += 55.0F;
                ui.slider(slider_id++, {22, y, parameter_width, 40}, "RED",
                          text.color[0], 0.0F, 1.0F, 2);
                y += 55.0F;
                ui.slider(slider_id++, {22, y, parameter_width, 40}, "GREEN",
                          text.color[1], 0.0F, 1.0F, 2);
                y += 55.0F;
                ui.slider(slider_id++, {22, y, parameter_width, 40}, "BLUE",
                          text.color[2], 0.0F, 1.0F, 2);
                y += 55.0F;

                ui.fill({22, y, parameter_width, 30},
                        SDL_Color{
                            static_cast<std::uint8_t>(
                                text.color[0] * 255.0F),
                            static_cast<std::uint8_t>(
                                text.color[1] * 255.0F),
                            static_cast<std::uint8_t>(
                                text.color[2] * 255.0F),
                            255,
                        });
                ui.text(28, y + 6, "HARD-EDGED 5x7 PIXEL FONT",
                        SDL_Color{15, 17, 24, 255});
                y += 43.0F;
                ui.text(22, y,
                        editing_text
                            ? "TYPE TEXT  |  ENTER OR ESC FINISHES"
                            : "CLICK THE TEXT FIELD TO EDIT",
                        muted);
                y += 28.0F;
            }
        }

        const auto rendered =
            audio.rendered_samples.load(std::memory_order_acquire);
        const auto generated =
            audio.generated_samples.load(std::memory_order_acquire);
        const auto latency = static_cast<std::uint64_t>(obtained.samples);
        const auto audible_position = audible_loop_position(
            generated, rendered, latency, song.samples_per_step(), song_loop);
        const auto sync = song.sync_at(audible_position);

        const float footer_y =
            std::max(y + 12.0F, static_cast<float>(controls_height) - 66.0F);
        ui.text(22, footer_y,
                "BEAT " + std::to_string(static_cast<int>(sync.beat) + 1) +
                    "   Ctrl+S/O/R: save/load/reload",
                muted);
        ui.text(22, footer_y + 28, status.message,
                status.error ? warning : healthy);

        if (toggle_playback) {
            playing = !playing;
            SDL_PauseAudioDevice(audio_device, playing ? 0 : 1);
        }
        if (request_save) {
            save_preset(settings, preset_path, status);
        }
        if (request_load) {
            load_preset(settings, preset_path, status);
            selected_text = settings.texts().empty()
                                ? 0
                                : std::min(selected_text,
                                           settings.texts().size() - 1);
            editing_text = false;
            SDL_StopTextInput();
        }
        if (request_reset) {
            settings.reset();
            status = {"Restored shader defaults", false};
        }
        if (request_reload) {
            SDL_GL_MakeCurrent(preview_window, preview_context);
            if (preview.reload() &&
                settings.load_schema(shader_path, true, error)) {
                status = {"Reloaded shader and parameter schema", false};
            } else {
                status = {preview.error().empty() ? error : preview.error(),
                          true};
            }
        }

        SDL_RenderPresent(controls_renderer);

        if (preview_visible) {
            SDL_GL_MakeCurrent(preview_window, preview_context);
            const std::uint32_t now = SDL_GetTicks();
            if (now - last_reload_check >= 200) {
                if (preview.reload_if_changed()) {
                    if (settings.load_schema(shader_path, true, error)) {
                        status = {"Shader reloaded automatically", false};
                    } else {
                        status = {error, true};
                    }
                } else if (!preview.error().empty()) {
                    status = {preview.error(), true};
                }
                last_reload_check = now;
            }

            int preview_width = 0;
            int preview_height = 0;
            SDL_GL_GetDrawableSize(preview_window, &preview_width,
                                   &preview_height);
            preview.render(preview_width, preview_height, sync,
                           settings.parameters(), settings.texts());
            SDL_GL_SwapWindow(preview_window);
        }
    }

    SDL_CloseAudioDevice(audio_device);
    SDL_StopTextInput();
    SDL_GL_MakeCurrent(preview_window, preview_context);
    preview.shutdown();
    SDL_GL_DeleteContext(preview_context);
    SDL_DestroyWindow(preview_window);
    ui.shutdown();
    SDL_DestroyRenderer(controls_renderer);
    SDL_DestroyWindow(controls_window);
    SDL_Quit();
    return 0;
}
