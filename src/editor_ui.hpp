#pragma once

#include <SDL2/SDL.h>

#include <array>
#include <filesystem>
#include <string>
#include <string_view>

namespace tiny::editor {

struct Rect {
    float x{0.0F};
    float y{0.0F};
    float width{0.0F};
    float height{0.0F};
};

struct Input {
    int mouse_x{0};
    int mouse_y{0};
    bool mouse_down{false};
    bool mouse_pressed{false};
    bool mouse_double_clicked{false};
    bool mouse_released{false};
    bool right_down{false};
    bool right_pressed{false};
    bool control_down{false};
    bool shift_down{false};
    int wheel_y{0};
};

class Font {
public:
    ~Font();

    bool load(SDL_Renderer* renderer, const std::filesystem::path& path,
              int pixel_height, std::string& error);
    void shutdown();
    void draw(SDL_Renderer* renderer, float x, float y, std::string_view text,
              SDL_Color color) const;
    [[nodiscard]] int width(std::string_view text) const;
    [[nodiscard]] int line_height() const;

private:
    struct Glyph {
        SDL_Rect source{};
        int bearing_x{0};
        int bearing_y{0};
        int advance{0};
    };

    SDL_Texture* texture_{nullptr};
    std::array<Glyph, 128> glyphs_{};
    int pixel_height_{0};
    int line_height_{0};
};

class Ui {
public:
    bool initialize(SDL_Renderer* renderer, const std::filesystem::path& font,
                    std::string& error);
    void shutdown();
    void begin_frame(Input input);

    void fill(Rect rect, SDL_Color color) const;
    void outline(Rect rect, SDL_Color color, int thickness = 1) const;
    void text(float x, float y, std::string_view value, SDL_Color color) const;
    [[nodiscard]] int text_width(std::string_view value) const;

    bool button(Rect rect, std::string_view label, bool selected = false);
    bool checkbox(Rect rect, std::string_view label, bool& value);
    bool slider(int id, Rect rect, std::string_view label, float& value,
                float minimum, float maximum, int decimals = 2);
    [[nodiscard]] bool hovered(Rect rect) const;
    [[nodiscard]] const Input& input() const;

private:
    SDL_Renderer* renderer_{nullptr};
    Font font_;
    Input input_{};
    int active_slider_{-1};
};

} // namespace tiny::editor
