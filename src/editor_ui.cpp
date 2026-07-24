#include "editor_ui.hpp"

#include <ft2build.h>
#include FT_FREETYPE_H

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdint>

namespace tiny::editor {
namespace {

SDL_Rect to_sdl_rect(Rect rect) {
    return {
        .x = static_cast<int>(std::lround(rect.x)),
        .y = static_cast<int>(std::lround(rect.y)),
        .w = static_cast<int>(std::lround(rect.width)),
        .h = static_cast<int>(std::lround(rect.height)),
    };
}

SDL_Color mix(SDL_Color first, SDL_Color second, float amount) {
    const auto channel = [amount](std::uint8_t a, std::uint8_t b) {
        return static_cast<std::uint8_t>(
            std::lround(static_cast<float>(a) * (1.0F - amount) +
                        static_cast<float>(b) * amount));
    };
    return {
        channel(first.r, second.r),
        channel(first.g, second.g),
        channel(first.b, second.b),
        channel(first.a, second.a),
    };
}

} // namespace

Font::~Font() {
    shutdown();
}

bool Font::load(SDL_Renderer* renderer, const std::filesystem::path& path,
                int pixel_height, std::string& error) {
    shutdown();

    FT_Library library = nullptr;
    if (FT_Init_FreeType(&library) != 0) {
        error = "FreeType could not initialize";
        return false;
    }

    FT_Face face = nullptr;
    if (FT_New_Face(library, path.c_str(), 0, &face) != 0) {
        error = "Could not load editor font: " + path.string();
        FT_Done_FreeType(library);
        return false;
    }

    FT_Set_Pixel_Sizes(face, 0, static_cast<FT_UInt>(pixel_height));
    pixel_height_ = pixel_height;
    line_height_ =
        std::max(pixel_height, static_cast<int>(face->size->metrics.height >> 6));

    constexpr int atlas_width = 1024;
    constexpr int atlas_height = 128;
    SDL_Surface* atlas =
        SDL_CreateRGBSurfaceWithFormat(0, atlas_width, atlas_height, 32,
                                       SDL_PIXELFORMAT_RGBA32);
    if (atlas == nullptr) {
        error = "Could not create font atlas";
        FT_Done_Face(face);
        FT_Done_FreeType(library);
        return false;
    }
    SDL_FillRect(atlas, nullptr, SDL_MapRGBA(atlas->format, 255, 255, 255, 0));

    int pen_x = 1;
    int pen_y = 1;
    int row_height = 0;
    for (unsigned int character = 32; character < 127; ++character) {
        if (FT_Load_Char(face, character, FT_LOAD_RENDER) != 0) {
            continue;
        }
        const FT_GlyphSlot glyph = face->glyph;
        const int glyph_width = static_cast<int>(glyph->bitmap.width);
        const int glyph_height = static_cast<int>(glyph->bitmap.rows);

        if (pen_x + glyph_width + 1 >= atlas_width) {
            pen_x = 1;
            pen_y += row_height + 1;
            row_height = 0;
        }
        if (pen_y + glyph_height >= atlas_height) {
            error = "The font atlas is too small";
            SDL_FreeSurface(atlas);
            FT_Done_Face(face);
            FT_Done_FreeType(library);
            return false;
        }

        auto& stored = glyphs_[character];
        stored.source = {pen_x, pen_y, glyph_width, glyph_height};
        stored.bearing_x = glyph->bitmap_left;
        stored.bearing_y = glyph->bitmap_top;
        stored.advance = static_cast<int>(glyph->advance.x >> 6);

        for (int row = 0; row < glyph_height; ++row) {
            for (int column = 0; column < glyph_width; ++column) {
                const auto alpha = glyph->bitmap.buffer[
                    row * static_cast<int>(glyph->bitmap.pitch) + column];
                auto* pixels = static_cast<std::uint32_t*>(atlas->pixels);
                const int stride = atlas->pitch / 4;
                pixels[(pen_y + row) * stride + pen_x + column] =
                    SDL_MapRGBA(atlas->format, 255, 255, 255, alpha);
            }
        }

        pen_x += glyph_width + 1;
        row_height = std::max(row_height, glyph_height);
    }

    texture_ = SDL_CreateTextureFromSurface(renderer, atlas);
    SDL_FreeSurface(atlas);
    FT_Done_Face(face);
    FT_Done_FreeType(library);

    if (texture_ == nullptr) {
        error = "Could not upload the font atlas";
        return false;
    }
    SDL_SetTextureBlendMode(texture_, SDL_BLENDMODE_BLEND);
    error.clear();
    return true;
}

void Font::shutdown() {
    if (texture_ != nullptr) {
        SDL_DestroyTexture(texture_);
        texture_ = nullptr;
    }
}

void Font::draw(SDL_Renderer* renderer, float x, float y, std::string_view text,
                SDL_Color color) const {
    if (texture_ == nullptr) {
        return;
    }

    SDL_SetTextureColorMod(texture_, color.r, color.g, color.b);
    SDL_SetTextureAlphaMod(texture_, color.a);

    int pen_x = static_cast<int>(std::lround(x));
    const int baseline =
        static_cast<int>(std::lround(y)) + static_cast<int>(pixel_height_ * 0.82F);
    for (const char value : text) {
        const auto character = static_cast<unsigned char>(value);
        if (character >= glyphs_.size()) {
            continue;
        }
        const auto& glyph = glyphs_[character];
        SDL_Rect destination{
            pen_x + glyph.bearing_x,
            baseline - glyph.bearing_y,
            glyph.source.w,
            glyph.source.h,
        };
        if (glyph.source.w > 0 && glyph.source.h > 0) {
            SDL_RenderCopy(renderer, texture_, &glyph.source, &destination);
        }
        pen_x += glyph.advance;
    }
}

int Font::width(std::string_view text) const {
    int result = 0;
    for (const char value : text) {
        const auto character = static_cast<unsigned char>(value);
        if (character < glyphs_.size()) {
            result += glyphs_[character].advance;
        }
    }
    return result;
}

int Font::line_height() const {
    return line_height_;
}

bool Ui::initialize(SDL_Renderer* renderer,
                    const std::filesystem::path& font,
                    std::string& error) {
    renderer_ = renderer;
    return font_.load(renderer, font, 17, error);
}

void Ui::shutdown() {
    font_.shutdown();
    renderer_ = nullptr;
}

void Ui::begin_frame(Input input) {
    input_ = input;
    if (input_.mouse_released) {
        active_slider_ = -1;
    }
}

void Ui::fill(Rect rect, SDL_Color color) const {
    const SDL_Rect destination = to_sdl_rect(rect);
    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer_, color.r, color.g, color.b, color.a);
    SDL_RenderFillRect(renderer_, &destination);
}

void Ui::outline(Rect rect, SDL_Color color, int thickness) const {
    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer_, color.r, color.g, color.b, color.a);
    for (int index = 0; index < thickness; ++index) {
        const SDL_Rect border = to_sdl_rect({
            rect.x + index,
            rect.y + index,
            rect.width - index * 2.0F,
            rect.height - index * 2.0F,
        });
        SDL_RenderDrawRect(renderer_, &border);
    }
}

void Ui::text(float x, float y, std::string_view value, SDL_Color color) const {
    font_.draw(renderer_, x, y, value, color);
}

int Ui::text_width(std::string_view value) const {
    return font_.width(value);
}

bool Ui::button(Rect rect, std::string_view label, bool selected) {
    constexpr SDL_Color base{38, 41, 57, 255};
    constexpr SDL_Color hover{54, 59, 80, 255};
    constexpr SDL_Color accent{101, 84, 214, 255};
    constexpr SDL_Color foreground{229, 231, 244, 255};

    SDL_Color color = selected ? accent : base;
    if (hovered(rect)) {
        color = mix(color, hover, 0.5F);
    }
    fill(rect, color);
    outline(rect, selected ? SDL_Color{150, 133, 255, 255}
                           : SDL_Color{72, 77, 100, 255});
    const int label_width = text_width(label);
    text(rect.x + (rect.width - static_cast<float>(label_width)) * 0.5F,
         rect.y + (rect.height - 17.0F) * 0.5F - 1.0F, label, foreground);
    return input_.mouse_pressed && hovered(rect);
}

bool Ui::checkbox(Rect rect, std::string_view label, bool& value) {
    constexpr SDL_Color foreground{218, 221, 238, 255};
    constexpr SDL_Color box{42, 46, 64, 255};
    constexpr SDL_Color border{91, 96, 122, 255};
    constexpr SDL_Color accent{116, 94, 232, 255};

    const Rect checkbox_box{
        rect.x,
        rect.y + (rect.height - 22.0F) * 0.5F,
        22,
        22,
    };
    fill(checkbox_box, value ? accent : box);
    outline(checkbox_box, value ? SDL_Color{190, 178, 255, 255} : border);
    if (value) {
        fill({checkbox_box.x + 5, checkbox_box.y + 10, 4, 4}, foreground);
        fill({checkbox_box.x + 9, checkbox_box.y + 7, 4, 7}, foreground);
        fill({checkbox_box.x + 13, checkbox_box.y + 4, 4, 7}, foreground);
    }
    text(rect.x + 34, rect.y + (rect.height - 17.0F) * 0.5F - 1.0F,
         label, foreground);

    if (input_.mouse_pressed && hovered(rect)) {
        value = !value;
        return true;
    }
    return false;
}

bool Ui::slider(int id, Rect rect, std::string_view label, float& value,
                float minimum, float maximum, int decimals) {
    constexpr SDL_Color foreground{218, 221, 238, 255};
    constexpr SDL_Color muted{118, 124, 151, 255};
    constexpr SDL_Color track{42, 46, 64, 255};
    constexpr SDL_Color accent{116, 94, 232, 255};

    char formatted[64]{};
    std::snprintf(formatted, sizeof(formatted), "%.*f", decimals,
                  static_cast<double>(value));
    text(rect.x, rect.y, label, foreground);
    text(rect.x + rect.width - static_cast<float>(text_width(formatted)),
         rect.y, formatted, muted);

    const Rect bar{rect.x, rect.y + 23.0F, rect.width, 11.0F};
    fill(bar, track);

    if (input_.mouse_pressed && hovered(bar)) {
        active_slider_ = id;
    }

    bool changed = false;
    if (active_slider_ == id && input_.mouse_down) {
        const float next =
            minimum + std::clamp((static_cast<float>(input_.mouse_x) - bar.x) /
                                     std::max(1.0F, bar.width),
                                 0.0F, 1.0F) *
                          (maximum - minimum);
        if (next != value) {
            value = next;
            changed = true;
        }
    }

    const float amount =
        std::clamp((value - minimum) / (maximum - minimum), 0.0F, 1.0F);
    fill({bar.x, bar.y, bar.width * amount, bar.height}, accent);
    fill({bar.x + bar.width * amount - 3.0F, bar.y - 3.0F, 6.0F,
          bar.height + 6.0F},
         SDL_Color{191, 178, 255, 255});
    return changed;
}

bool Ui::hovered(Rect rect) const {
    return static_cast<float>(input_.mouse_x) >= rect.x &&
           static_cast<float>(input_.mouse_x) < rect.x + rect.width &&
           static_cast<float>(input_.mouse_y) >= rect.y &&
           static_cast<float>(input_.mouse_y) < rect.y + rect.height;
}

const Input& Ui::input() const {
    return input_;
}

} // namespace tiny::editor
