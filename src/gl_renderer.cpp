#define GL_GLEXT_PROTOTYPES
#include <SDL2/SDL_opengl.h>

#include "gl_renderer.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <utility>
#include <vector>

namespace tiny {
namespace {

constexpr const char* vertex_source = R"(
#version 330 core

void main() {
    vec2 position = vec2(
        float((gl_VertexID << 1) & 2),
        float(gl_VertexID & 2)
    );
    gl_Position = vec4(position * 2.0 - 1.0, 0.0, 1.0);
}
)";

constexpr const char* text_vertex_source = R"(
#version 330 core

layout(location = 0) in vec2 a_position;
layout(location = 1) in vec4 a_color;

uniform vec2 u_resolution;
out vec4 v_color;

void main() {
    vec2 normalized = a_position / u_resolution;
    vec2 clip = vec2(normalized.x * 2.0 - 1.0,
                     1.0 - normalized.y * 2.0);
    gl_Position = vec4(clip, 0.0, 1.0);
    v_color = a_color;
}
)";

constexpr const char* text_fragment_source = R"(
#version 330 core

in vec4 v_color;
out vec4 out_color;

void main() {
    out_color = v_color;
}
)";

using Glyph = std::array<std::uint8_t, 7>;

Glyph glyph_rows(char value) {
    const char glyph =
        static_cast<char>(std::toupper(static_cast<unsigned char>(value)));
    switch (glyph) {
    case 'A': return {0b01110, 0b10001, 0b10001, 0b11111, 0b10001, 0b10001, 0b10001};
    case 'B': return {0b11110, 0b10001, 0b10001, 0b11110, 0b10001, 0b10001, 0b11110};
    case 'C': return {0b01111, 0b10000, 0b10000, 0b10000, 0b10000, 0b10000, 0b01111};
    case 'D': return {0b11110, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b11110};
    case 'E': return {0b11111, 0b10000, 0b10000, 0b11110, 0b10000, 0b10000, 0b11111};
    case 'F': return {0b11111, 0b10000, 0b10000, 0b11110, 0b10000, 0b10000, 0b10000};
    case 'G': return {0b01111, 0b10000, 0b10000, 0b10111, 0b10001, 0b10001, 0b01111};
    case 'H': return {0b10001, 0b10001, 0b10001, 0b11111, 0b10001, 0b10001, 0b10001};
    case 'I': return {0b11111, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b11111};
    case 'J': return {0b00111, 0b00010, 0b00010, 0b00010, 0b10010, 0b10010, 0b01100};
    case 'K': return {0b10001, 0b10010, 0b10100, 0b11000, 0b10100, 0b10010, 0b10001};
    case 'L': return {0b10000, 0b10000, 0b10000, 0b10000, 0b10000, 0b10000, 0b11111};
    case 'M': return {0b10001, 0b11011, 0b10101, 0b10101, 0b10001, 0b10001, 0b10001};
    case 'N': return {0b10001, 0b11001, 0b10101, 0b10011, 0b10001, 0b10001, 0b10001};
    case 'O': return {0b01110, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b01110};
    case 'P': return {0b11110, 0b10001, 0b10001, 0b11110, 0b10000, 0b10000, 0b10000};
    case 'Q': return {0b01110, 0b10001, 0b10001, 0b10001, 0b10101, 0b10010, 0b01101};
    case 'R': return {0b11110, 0b10001, 0b10001, 0b11110, 0b10100, 0b10010, 0b10001};
    case 'S': return {0b01111, 0b10000, 0b10000, 0b01110, 0b00001, 0b00001, 0b11110};
    case 'T': return {0b11111, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100};
    case 'U': return {0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b01110};
    case 'V': return {0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b01010, 0b00100};
    case 'W': return {0b10001, 0b10001, 0b10001, 0b10101, 0b10101, 0b10101, 0b01010};
    case 'X': return {0b10001, 0b10001, 0b01010, 0b00100, 0b01010, 0b10001, 0b10001};
    case 'Y': return {0b10001, 0b10001, 0b01010, 0b00100, 0b00100, 0b00100, 0b00100};
    case 'Z': return {0b11111, 0b00001, 0b00010, 0b00100, 0b01000, 0b10000, 0b11111};
    case '0': return {0b01110, 0b10001, 0b10011, 0b10101, 0b11001, 0b10001, 0b01110};
    case '1': return {0b00100, 0b01100, 0b00100, 0b00100, 0b00100, 0b00100, 0b01110};
    case '2': return {0b01110, 0b10001, 0b00001, 0b00010, 0b00100, 0b01000, 0b11111};
    case '3': return {0b11110, 0b00001, 0b00001, 0b01110, 0b00001, 0b00001, 0b11110};
    case '4': return {0b00010, 0b00110, 0b01010, 0b10010, 0b11111, 0b00010, 0b00010};
    case '5': return {0b11111, 0b10000, 0b10000, 0b11110, 0b00001, 0b00001, 0b11110};
    case '6': return {0b01110, 0b10000, 0b10000, 0b11110, 0b10001, 0b10001, 0b01110};
    case '7': return {0b11111, 0b00001, 0b00010, 0b00100, 0b01000, 0b01000, 0b01000};
    case '8': return {0b01110, 0b10001, 0b10001, 0b01110, 0b10001, 0b10001, 0b01110};
    case '9': return {0b01110, 0b10001, 0b10001, 0b01111, 0b00001, 0b00001, 0b01110};
    case '!': return {0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b00000, 0b00100};
    case '?': return {0b01110, 0b10001, 0b00001, 0b00010, 0b00100, 0b00000, 0b00100};
    case '.': return {0b00000, 0b00000, 0b00000, 0b00000, 0b00000, 0b00110, 0b00110};
    case ',': return {0b00000, 0b00000, 0b00000, 0b00000, 0b00110, 0b00100, 0b01000};
    case ':': return {0b00000, 0b00110, 0b00110, 0b00000, 0b00110, 0b00110, 0b00000};
    case ';': return {0b00000, 0b00110, 0b00110, 0b00000, 0b00110, 0b00100, 0b01000};
    case '-': return {0b00000, 0b00000, 0b00000, 0b11111, 0b00000, 0b00000, 0b00000};
    case '+': return {0b00000, 0b00100, 0b00100, 0b11111, 0b00100, 0b00100, 0b00000};
    case '/': return {0b00001, 0b00010, 0b00010, 0b00100, 0b01000, 0b01000, 0b10000};
    case '\'': return {0b00100, 0b00100, 0b00010, 0b00000, 0b00000, 0b00000, 0b00000};
    case '"': return {0b01010, 0b01010, 0b00100, 0b00000, 0b00000, 0b00000, 0b00000};
    case '(': return {0b00010, 0b00100, 0b01000, 0b01000, 0b01000, 0b00100, 0b00010};
    case ')': return {0b01000, 0b00100, 0b00010, 0b00010, 0b00010, 0b00100, 0b01000};
    case '=': return {0b00000, 0b11111, 0b00000, 0b11111, 0b00000, 0b00000, 0b00000};
    case '_': return {0b00000, 0b00000, 0b00000, 0b00000, 0b00000, 0b00000, 0b11111};
    case ' ': return {};
    default: return {0b01110, 0b10001, 0b00010, 0b00100, 0b00100, 0b00000, 0b00100};
    }
}

struct TextVertex {
    float x;
    float y;
    float red;
    float green;
    float blue;
    float alpha;
};

void append_pixel(std::vector<TextVertex>& vertices, float x, float y,
                  float size, const std::array<float, 3>& color,
                  float alpha) {
    const TextVertex top_left{x, y, color[0], color[1], color[2], alpha};
    const TextVertex top_right{x + size, y, color[0], color[1], color[2],
                               alpha};
    const TextVertex bottom_left{x, y + size, color[0], color[1], color[2],
                                 alpha};
    const TextVertex bottom_right{x + size, y + size, color[0], color[1],
                                  color[2], alpha};
    vertices.insert(vertices.end(),
                    {top_left, bottom_left, top_right, top_right, bottom_left,
                     bottom_right});
}

std::string read_text_file(const std::filesystem::path& path) {
    std::ifstream stream(path);
    if (!stream) {
        return {};
    }
    std::ostringstream contents;
    contents << stream.rdbuf();
    return contents.str();
}

} // namespace

GlRenderer::~GlRenderer() {
    shutdown();
}

void GlRenderer::shutdown() {
    if (program_ != 0) {
        glDeleteProgram(program_);
        program_ = 0;
    }
    if (vertex_array_ != 0) {
        glDeleteVertexArrays(1, &vertex_array_);
        vertex_array_ = 0;
    }
    if (text_program_ != 0) {
        glDeleteProgram(text_program_);
        text_program_ = 0;
    }
    if (text_vertex_buffer_ != 0) {
        glDeleteBuffers(1, &text_vertex_buffer_);
        text_vertex_buffer_ = 0;
    }
    if (text_vertex_array_ != 0) {
        glDeleteVertexArrays(1, &text_vertex_array_);
        text_vertex_array_ = 0;
    }
}

bool GlRenderer::initialize(std::filesystem::path fragment_path) {
    fragment_path_ = std::move(fragment_path);
    if (vertex_array_ == 0) {
        glGenVertexArrays(1, &vertex_array_);
    }
    glBindVertexArray(vertex_array_);
    return rebuild() && initialize_text_renderer();
}

bool GlRenderer::reload() {
    return rebuild();
}

bool GlRenderer::reload_if_changed() {
    std::error_code error_code;
    const auto write_time =
        std::filesystem::last_write_time(fragment_path_, error_code);
    if (!error_code && write_time != last_write_) {
        return rebuild();
    }
    return false;
}

void GlRenderer::render(
    int width, int height, const SyncState& sync,
    std::span<const EffectParameter> parameters,
    std::span<const TextOverlay> texts, bool effect_active) {
    if (program_ == 0) {
        return;
    }

    glViewport(0, 0, width, height);
    if (!effect_active) {
        glClearColor(0.0F, 0.0F, 0.0F, 1.0F);
        glClear(GL_COLOR_BUFFER_BIT);
        return;
    }
    glUseProgram(program_);
    glBindVertexArray(vertex_array_);

    glUniform2f(glGetUniformLocation(program_, "u_resolution"),
                static_cast<float>(width), static_cast<float>(height));
    glUniform1f(glGetUniformLocation(program_, "u_time"),
                static_cast<float>(sync.seconds));
    glUniform1f(glGetUniformLocation(program_, "u_beat"),
                static_cast<float>(sync.beat));
    glUniform1f(glGetUniformLocation(program_, "u_beat_phase"),
                sync.beat_phase);
    glUniform1f(glGetUniformLocation(program_, "u_bar_phase"),
                sync.bar_phase);
    glUniform1f(glGetUniformLocation(program_, "u_pulse"), sync.pulse);
    for (const auto& parameter : parameters) {
        glUniform1f(glGetUniformLocation(program_, parameter.uniform.c_str()),
                    parameter.value);
    }

    glDrawArrays(GL_TRIANGLES, 0, 3);
    render_texts(width, height, texts);
}

const std::string& GlRenderer::error() const {
    return error_;
}

bool GlRenderer::rebuild() {
    const std::string fragment_source = read_text_file(fragment_path_);
    if (fragment_source.empty()) {
        error_ = "Could not read fragment shader: " + fragment_path_.string();
        return false;
    }

    std::string compile_error;
    const GLuint vertex =
        compile_shader(GL_VERTEX_SHADER, vertex_source, compile_error);
    if (vertex == 0) {
        error_ = compile_error;
        return false;
    }

    const GLuint fragment =
        compile_shader(GL_FRAGMENT_SHADER, fragment_source, compile_error);
    if (fragment == 0) {
        glDeleteShader(vertex);
        error_ = compile_error;
        return false;
    }

    const GLuint next_program = glCreateProgram();
    glAttachShader(next_program, vertex);
    glAttachShader(next_program, fragment);
    glLinkProgram(next_program);
    glDeleteShader(vertex);
    glDeleteShader(fragment);

    GLint linked = GL_FALSE;
    glGetProgramiv(next_program, GL_LINK_STATUS, &linked);
    if (linked != GL_TRUE) {
        GLint length = 0;
        glGetProgramiv(next_program, GL_INFO_LOG_LENGTH, &length);
        std::vector<char> log(static_cast<std::size_t>(length) + 1);
        glGetProgramInfoLog(next_program, length, nullptr, log.data());
        error_ = "Shader link failed:\n" + std::string(log.data());
        glDeleteProgram(next_program);
        return false;
    }

    if (program_ != 0) {
        glDeleteProgram(program_);
    }
    program_ = next_program;
    std::error_code error_code;
    last_write_ = std::filesystem::last_write_time(fragment_path_, error_code);
    error_.clear();
    return true;
}

bool GlRenderer::initialize_text_renderer() {
    if (text_program_ != 0) {
        return true;
    }

    std::string compile_error;
    const GLuint vertex =
        compile_shader(GL_VERTEX_SHADER, text_vertex_source, compile_error);
    if (vertex == 0) {
        error_ = compile_error;
        return false;
    }
    const GLuint fragment =
        compile_shader(GL_FRAGMENT_SHADER, text_fragment_source, compile_error);
    if (fragment == 0) {
        glDeleteShader(vertex);
        error_ = compile_error;
        return false;
    }

    text_program_ = glCreateProgram();
    glAttachShader(text_program_, vertex);
    glAttachShader(text_program_, fragment);
    glLinkProgram(text_program_);
    glDeleteShader(vertex);
    glDeleteShader(fragment);

    GLint linked = GL_FALSE;
    glGetProgramiv(text_program_, GL_LINK_STATUS, &linked);
    if (linked != GL_TRUE) {
        GLint length = 0;
        glGetProgramiv(text_program_, GL_INFO_LOG_LENGTH, &length);
        std::vector<char> log(static_cast<std::size_t>(length) + 1);
        glGetProgramInfoLog(text_program_, length, nullptr, log.data());
        error_ = "Text shader link failed:\n" + std::string(log.data());
        glDeleteProgram(text_program_);
        text_program_ = 0;
        return false;
    }

    glGenVertexArrays(1, &text_vertex_array_);
    glGenBuffers(1, &text_vertex_buffer_);
    glBindVertexArray(text_vertex_array_);
    glBindBuffer(GL_ARRAY_BUFFER, text_vertex_buffer_);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(TextVertex),
                          reinterpret_cast<void*>(0));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(
        1, 4, GL_FLOAT, GL_FALSE, sizeof(TextVertex),
        reinterpret_cast<void*>(2 * sizeof(float)));
    glBindVertexArray(vertex_array_);
    error_.clear();
    return true;
}

void GlRenderer::render_texts(int width, int height,
                              std::span<const TextOverlay> texts) {
    if (text_program_ == 0 || texts.empty()) {
        return;
    }

    std::vector<TextVertex> vertices;
    for (const auto& text : texts) {
        if (!text.enabled || text.text.empty()) {
            continue;
        }

        const float pixel_size = text_pixel_size_for_viewport(
            text.scale, static_cast<float>(width),
            static_cast<float>(height));
        const float origin_x =
            std::round(text.x * static_cast<float>(width));
        const float origin_y =
            std::round(text.y * static_cast<float>(height));

        auto append_text_pass =
            [&](float offset_x, float offset_y,
                const std::array<float, 3>& color, float alpha) {
                float cursor_x = origin_x + offset_x;
                float cursor_y = origin_y + offset_y;
                for (const char character : text.text) {
                    if (character == '\n') {
                        cursor_x = origin_x + offset_x;
                        cursor_y += pixel_size * 8.0F;
                        continue;
                    }
                    const auto rows = glyph_rows(character);
                    for (std::size_t row = 0; row < rows.size(); ++row) {
                        for (std::size_t column = 0; column < 5; ++column) {
                            const auto bit =
                                static_cast<std::uint8_t>(1U << (4U - column));
                            if ((rows[row] & bit) == 0) {
                                continue;
                            }
                            append_pixel(
                                vertices,
                                cursor_x +
                                    static_cast<float>(column) * pixel_size,
                                cursor_y + static_cast<float>(row) * pixel_size,
                                pixel_size, color, alpha);
                        }
                    }
                    cursor_x += pixel_size * 6.0F;
                }
            };

        append_text_pass(pixel_size, pixel_size, {0.0F, 0.0F, 0.0F}, 0.72F);
        append_text_pass(0.0F, 0.0F, text.color, 1.0F);
    }

    if (vertices.empty()) {
        return;
    }

    glUseProgram(text_program_);
    glUniform2f(glGetUniformLocation(text_program_, "u_resolution"),
                static_cast<float>(width), static_cast<float>(height));
    glBindVertexArray(text_vertex_array_);
    glBindBuffer(GL_ARRAY_BUFFER, text_vertex_buffer_);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(vertices.size() *
                                        sizeof(TextVertex)),
                 vertices.data(), GL_STREAM_DRAW);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(vertices.size()));
    glDisable(GL_BLEND);
    glBindVertexArray(vertex_array_);
}

GLuint GlRenderer::compile_shader(unsigned int type, const std::string& source,
                                  std::string& error) {
    const GLuint shader = glCreateShader(type);
    const char* source_pointer = source.c_str();
    glShaderSource(shader, 1, &source_pointer, nullptr);
    glCompileShader(shader);

    GLint compiled = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (compiled == GL_TRUE) {
        return shader;
    }

    GLint length = 0;
    glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &length);
    std::vector<char> log(static_cast<std::size_t>(length) + 1);
    glGetShaderInfoLog(shader, length, nullptr, log.data());
    error = "Shader compilation failed:\n" + std::string(log.data());
    glDeleteShader(shader);
    return 0;
}

} // namespace tiny
