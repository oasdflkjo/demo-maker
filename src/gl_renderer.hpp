#pragma once

#include "tiny/effect.hpp"
#include "tiny/song.hpp"

#include <filesystem>
#include <string>

using GLuint = unsigned int;

namespace tiny {

class GlRenderer {
public:
    ~GlRenderer();

    bool initialize(std::filesystem::path fragment_path);
    bool reload();
    bool reload_if_changed();
    void render(int width, int height, const SyncState& sync,
                std::span<const EffectParameter> parameters = {},
                std::span<const TextOverlay> texts = {});
    void shutdown();
    [[nodiscard]] const std::string& error() const;

private:
    bool rebuild();
    bool initialize_text_renderer();
    void render_texts(int width, int height,
                      std::span<const TextOverlay> texts);
    static GLuint compile_shader(unsigned int type, const std::string& source,
                                 std::string& error);

    std::filesystem::path fragment_path_;
    std::filesystem::file_time_type last_write_{};
    GLuint program_{0};
    GLuint vertex_array_{0};
    GLuint text_program_{0};
    GLuint text_vertex_array_{0};
    GLuint text_vertex_buffer_{0};
    std::string error_;
};

} // namespace tiny
