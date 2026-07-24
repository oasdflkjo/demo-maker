#pragma once

#include <array>
#include <cstddef>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace tiny {

struct EffectParameter {
    std::string uniform;
    std::string label;
    float default_value{0.0F};
    float value{0.0F};
    float minimum{0.0F};
    float maximum{1.0F};
    float step{0.01F};
};

struct TextOverlay {
    std::string text{"HELLO FROM TINY DEMO"};
    float x{0.10F};
    float y{0.18F};
    float scale{5.0F};
    std::array<float, 3> color{0.55F, 0.85F, 1.0F};
    bool enabled{true};
};

void center_text_overlay(TextOverlay& text, float viewport_width,
                         float viewport_height);

class EffectSettings {
public:
    bool load_schema(const std::filesystem::path& shader_path,
                     bool preserve_values, std::string& error);
    bool save_preset(const std::filesystem::path& path, std::string& error) const;
    bool load_preset(const std::filesystem::path& path, std::string& error);
    void reset();

    [[nodiscard]] std::span<const EffectParameter> parameters() const;
    [[nodiscard]] std::span<EffectParameter> parameters();
    [[nodiscard]] std::span<const TextOverlay> texts() const;
    [[nodiscard]] std::span<TextOverlay> texts();
    std::size_t add_text(TextOverlay text = {});
    void remove_text(std::size_t index);

private:
    std::vector<EffectParameter> parameters_;
    std::vector<TextOverlay> texts_;
};

} // namespace tiny
