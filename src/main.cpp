#include "gl_renderer.hpp"
#include "tiny/project_io.hpp"
#include "tiny/synth.hpp"

#include <SDL2/SDL.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

struct AudioState {
    explicit AudioState(tiny::Song song) : synth(std::move(song)) {
        const auto loop = synth.song().populated_range();
        synth.set_loop_steps(loop.start, loop.length);
    }

    tiny::SynthEngine synth;
    std::atomic<std::uint64_t> generated_samples{0};
};

void audio_callback(void* user_data, std::uint8_t* stream, int byte_count) {
    auto& state = *static_cast<AudioState*>(user_data);
    auto* samples = reinterpret_cast<float*>(stream);
    const auto sample_count =
        static_cast<std::size_t>(byte_count) / sizeof(float);
    state.synth.render(std::span<float>(samples, sample_count));
    state.generated_samples.store(state.synth.sample_position(),
                                  std::memory_order_release);
}

void write_u16(std::ofstream& output, std::uint16_t value) {
    output.put(static_cast<char>(value & 0xffU));
    output.put(static_cast<char>((value >> 8U) & 0xffU));
}

void write_u32(std::ofstream& output, std::uint32_t value) {
    write_u16(output, static_cast<std::uint16_t>(value & 0xffffU));
    write_u16(output, static_cast<std::uint16_t>((value >> 16U) & 0xffffU));
}

bool render_wav(const std::filesystem::path& path, float seconds,
                tiny::Song song) {
    tiny::SynthEngine synth(std::move(song));
    const auto loop = synth.song().populated_range();
    synth.set_loop_steps(loop.start, loop.length);
    const auto frame_count =
        static_cast<std::size_t>(seconds * static_cast<float>(tiny::sample_rate));
    std::vector<float> samples(frame_count * 2);
    synth.render(samples);

    std::ofstream output(path, std::ios::binary);
    if (!output) {
        std::cerr << "Could not open WAV output: " << path << '\n';
        return false;
    }

    constexpr std::uint16_t channels = 2;
    constexpr std::uint16_t bits_per_sample = 16;
    const auto data_size =
        static_cast<std::uint32_t>(frame_count * channels * sizeof(std::int16_t));

    output.write("RIFF", 4);
    write_u32(output, 36U + data_size);
    output.write("WAVEfmt ", 8);
    write_u32(output, 16);
    write_u16(output, 1);
    write_u16(output, channels);
    write_u32(output, tiny::sample_rate);
    write_u32(output, tiny::sample_rate * channels * bits_per_sample / 8U);
    write_u16(output, channels * bits_per_sample / 8U);
    write_u16(output, bits_per_sample);
    output.write("data", 4);
    write_u32(output, data_size);

    for (const float sample : samples) {
        const float clamped = std::clamp(sample, -1.0F, 1.0F);
        const auto pcm = static_cast<std::int16_t>(
            std::lrint(clamped * static_cast<float>(INT16_MAX)));
        write_u16(output, static_cast<std::uint16_t>(pcm));
    }

    std::cout << "Rendered " << seconds << " seconds to " << path << '\n';
    return true;
}

void print_help(std::string_view executable) {
    std::cout << "Usage:\n"
              << "  " << executable
              << " [--shader FILE] [--effect FILE] [--song FILE]\n"
              << "  " << executable
              << " --render-wav FILE [SECONDS] [--song FILE]\n\n"
              << "Controls: Escape quits, Space pauses, R reloads the shader.\n";
}

} // namespace

int main(int argc, char** argv) {
    std::filesystem::path shader_path = TINY_DEMO_SHADER_PATH;
    std::optional<std::filesystem::path> song_path;
    std::optional<std::filesystem::path> effect_path;
    std::optional<std::filesystem::path> wav_path;
    float render_seconds = 16.0F;

    for (int index = 1; index < argc; ++index) {
        const std::string_view argument = argv[index];
        if (argument == "--help" || argument == "-h") {
            print_help(argv[0]);
            return 0;
        }
        if (argument == "--shader" && index + 1 < argc) {
            shader_path = argv[++index];
            continue;
        }
        if (argument == "--song" && index + 1 < argc) {
            song_path = argv[++index];
            continue;
        }
        if (argument == "--effect" && index + 1 < argc) {
            effect_path = argv[++index];
            continue;
        }
        if (argument == "--render-wav" && index + 1 < argc) {
            wav_path = argv[++index];
            const std::string_view next =
                index + 1 < argc ? std::string_view(argv[index + 1])
                                 : std::string_view{};
            if (!next.empty() && next.front() != '-') {
                render_seconds = std::stof(argv[++index]);
            }
            continue;
        }
        std::cerr << "Unknown or incomplete argument: " << argument << '\n';
        print_help(argv[0]);
        return 1;
    }

    tiny::Song song = tiny::Song::make_demo_song();
    if (song_path.has_value()) {
        std::string error;
        if (!tiny::load_song_binary(*song_path, song, error)) {
            std::cerr << error << '\n';
            return 1;
        }
    }
    if (wav_path.has_value()) {
        return render_wav(*wav_path, render_seconds, song) ? 0 : 1;
    }

    tiny::EffectSettings effect;
    std::string effect_error;
    if (!effect.load_schema(shader_path, false, effect_error)) {
        std::cerr << effect_error << '\n';
        return 1;
    }
    if (effect_path.has_value() &&
        !effect.load_preset(*effect_path, effect_error)) {
        std::cerr << effect_error << '\n';
        return 1;
    }

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0) {
        std::cerr << "SDL initialization failed: " << SDL_GetError() << '\n';
        return 1;
    }

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,
                        SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

    SDL_Window* window = SDL_CreateWindow(
        "Demo Maker — Runtime", SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED, 1280, 720,
        SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (window == nullptr) {
        std::cerr << "Window creation failed: " << SDL_GetError() << '\n';
        SDL_Quit();
        return 1;
    }

    SDL_GLContext gl_context = SDL_GL_CreateContext(window);
    if (gl_context == nullptr) {
        std::cerr << "OpenGL context creation failed: " << SDL_GetError()
                  << '\n';
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    SDL_GL_SetSwapInterval(1);

    tiny::GlRenderer renderer;
    if (!renderer.initialize(shader_path)) {
        std::cerr << renderer.error() << '\n';
    }

    AudioState audio(song);
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
        renderer.shutdown();
        SDL_GL_DeleteContext(gl_context);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    SDL_PauseAudioDevice(audio_device, 0);

    bool running = true;
    bool paused = false;
    std::uint32_t last_reload_check = 0;

    while (running) {
        SDL_Event event{};
        while (SDL_PollEvent(&event) != 0) {
            if (event.type == SDL_QUIT) {
                running = false;
            }
            if (event.type == SDL_KEYDOWN && event.key.repeat == 0) {
                if (event.key.keysym.sym == SDLK_ESCAPE) {
                    running = false;
                } else if (event.key.keysym.sym == SDLK_SPACE) {
                    paused = !paused;
                    SDL_PauseAudioDevice(audio_device, paused ? 1 : 0);
                } else if (event.key.keysym.sym == SDLK_r) {
                    if (renderer.reload()) {
                        effect.load_schema(shader_path, true, effect_error);
                    }
                    if (!renderer.error().empty()) {
                        std::cerr << renderer.error() << '\n';
                    }
                }
            }
        }

        const std::uint32_t now = SDL_GetTicks();
        if (now - last_reload_check >= 200) {
            if (renderer.reload_if_changed()) {
                effect.load_schema(shader_path, true, effect_error);
            }
            if (!renderer.error().empty()) {
                std::cerr << renderer.error() << '\n';
            }
            last_reload_check = now;
        }

        int width = 0;
        int height = 0;
        SDL_GL_GetDrawableSize(window, &width, &height);

        const auto generated =
            audio.generated_samples.load(std::memory_order_acquire);
        const auto latency = static_cast<std::uint64_t>(obtained.samples);
        const auto audible_position =
            generated > latency ? generated - latency : 0;
        const auto sync = song.sync_at(audible_position);

        renderer.render(width, height, sync, effect.parameters(),
                        effect.texts());
        SDL_GL_SwapWindow(window);
    }

    SDL_CloseAudioDevice(audio_device);
    renderer.shutdown();
    SDL_GL_DeleteContext(gl_context);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
