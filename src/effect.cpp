#include "tiny/effect.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <unordered_map>

namespace tiny {
namespace {

constexpr std::uint32_t preset_version = 1;
constexpr std::string_view annotation = "// @param";

EffectParameter* find_parameter(std::vector<EffectParameter>& parameters,
                                const std::string& uniform) {
    const auto found = std::ranges::find(
        parameters, uniform, &EffectParameter::uniform);
    return found == parameters.end() ? nullptr : &*found;
}

} // namespace

float text_pixel_size_for_viewport(float scale, float viewport_width,
                                   float viewport_height) {
    if (viewport_width <= 0.0F || viewport_height <= 0.0F) {
        return 0.0F;
    }

    const float canvas_scale =
        std::min(viewport_width / effect_canvas_width,
                 viewport_height / effect_canvas_height);
    return std::clamp(std::round(scale), 1.0F, 16.0F) * canvas_scale;
}

void center_text_overlay(TextOverlay& text) {
    std::size_t line_count = 1;
    std::size_t current_columns = 0;
    std::size_t maximum_columns = 0;
    for (const char character : text.text) {
        if (character == '\n') {
            maximum_columns = std::max(maximum_columns, current_columns);
            current_columns = 0;
            ++line_count;
        } else {
            ++current_columns;
        }
    }
    maximum_columns = std::max(maximum_columns, current_columns);

    const float pixel_size = text_pixel_size_for_viewport(
        text.scale, effect_canvas_width, effect_canvas_height);
    // Each glyph advances six pixels and each line advances eight. The final
    // spacing pixel is occupied by the one-pixel shadow in the renderer.
    const float text_width =
        static_cast<float>(maximum_columns * 6) * pixel_size;
    const float text_height =
        static_cast<float>(line_count * 8) * pixel_size;
    text.x = std::max(
        0.0F, (effect_canvas_width - text_width) * 0.5F /
                  effect_canvas_width);
    text.y = std::max(
        0.0F, (effect_canvas_height - text_height) * 0.5F /
                  effect_canvas_height);
}

bool EffectSettings::load_schema(const std::filesystem::path& shader_path,
                                 bool preserve_values, std::string& error) {
    std::ifstream input(shader_path);
    if (!input) {
        error = "Could not read shader schema: " + shader_path.string();
        return false;
    }

    std::unordered_map<std::string, float> previous;
    if (preserve_values) {
        for (const auto& parameter : parameters_) {
            previous.emplace(parameter.uniform, parameter.value);
        }
    }

    std::vector<EffectParameter> parsed;
    std::string line;
    std::size_t line_number = 0;
    while (std::getline(input, line)) {
        ++line_number;
        const auto marker = line.find(annotation);
        if (marker == std::string::npos) {
            continue;
        }

        std::istringstream fields(
            line.substr(marker + annotation.size()));
        EffectParameter parameter;
        if (!(fields >> parameter.uniform >> std::quoted(parameter.label) >>
              parameter.default_value >> parameter.minimum >>
              parameter.maximum >> parameter.step) ||
            parameter.uniform.empty() || parameter.label.empty() ||
            parameter.minimum >= parameter.maximum || parameter.step <= 0.0F) {
            error = "Invalid @param annotation at " + shader_path.string() +
                    ":" + std::to_string(line_number);
            return false;
        }

        parameter.default_value = std::clamp(
            parameter.default_value, parameter.minimum, parameter.maximum);
        parameter.value = parameter.default_value;
        if (const auto old = previous.find(parameter.uniform);
            old != previous.end()) {
            parameter.value =
                std::clamp(old->second, parameter.minimum, parameter.maximum);
        }
        parsed.push_back(std::move(parameter));
    }

    if (parsed.empty()) {
        error = "Shader has no // @param annotations: " +
                shader_path.string();
        return false;
    }

    parameters_ = std::move(parsed);
    error.clear();
    return true;
}

bool EffectSettings::save_preset(const std::filesystem::path& path,
                                 std::string& error) const {
    std::ofstream output(path);
    if (!output) {
        error = "Could not open effect preset for writing: " + path.string();
        return false;
    }

    output << "TINY_DEMO_EFFECT " << preset_version << '\n'
           << std::setprecision(9);
    for (const auto& parameter : parameters_) {
        output << "value " << parameter.uniform << ' ' << parameter.value
               << '\n';
    }
    for (const auto& text : texts_) {
        output << "text " << (text.enabled ? 1 : 0) << ' '
               << std::quoted(text.text) << ' ' << text.x << ' ' << text.y
               << ' ' << text.scale << ' ' << text.color[0] << ' '
               << text.color[1] << ' ' << text.color[2] << '\n';
    }
    for (const auto& clip : clips_) {
        output << "clip " << (clip.enabled ? 1 : 0) << ' '
               << std::quoted(clip.name) << ' ' << clip.start_step << ' '
               << clip.length_steps << '\n';
    }
    output << "end\n";

    if (!output) {
        error = "Failed while writing effect preset: " + path.string();
        return false;
    }
    error.clear();
    return true;
}

bool EffectSettings::load_preset(const std::filesystem::path& path,
                                 std::string& error) {
    std::ifstream input(path);
    if (!input) {
        error = "Could not open effect preset: " + path.string();
        return false;
    }

    std::string magic;
    std::uint32_t version = 0;
    if (!(input >> magic >> version) || magic != "TINY_DEMO_EFFECT" ||
        version != preset_version) {
        error = "Unsupported effect preset: " + path.string();
        return false;
    }

    auto updated = parameters_;
    std::vector<TextOverlay> updated_texts;
    std::vector<EffectClip> updated_clips;
    std::string command;
    bool reached_end = false;
    while (input >> command) {
        if (command == "end") {
            reached_end = true;
            break;
        }
        if (command == "text") {
            TextOverlay text;
            int enabled = 0;
            if (!(input >> enabled >> std::quoted(text.text) >> text.x >>
                  text.y >> text.scale >> text.color[0] >> text.color[1] >>
                  text.color[2]) ||
                (enabled != 0 && enabled != 1) || text.text.empty() ||
                text.text.size() > 128 || !std::isfinite(text.x) ||
                !std::isfinite(text.y) || !std::isfinite(text.scale) ||
                !std::ranges::all_of(text.color, [](float component) {
                    return std::isfinite(component);
                })) {
                error = "Invalid text overlay in effect preset";
                return false;
            }
            text.enabled = enabled != 0;
            text.x = std::clamp(text.x, 0.0F, 1.0F);
            text.y = std::clamp(text.y, 0.0F, 1.0F);
            text.scale = std::clamp(text.scale, 1.0F, 16.0F);
            for (auto& component : text.color) {
                component = std::clamp(component, 0.0F, 1.0F);
            }
            updated_texts.push_back(std::move(text));
            continue;
        }
        if (command == "clip") {
            EffectClip clip;
            int enabled = 0;
            std::size_t start = 0;
            std::size_t length = 0;
            if (!(input >> enabled >> std::quoted(clip.name) >> start >>
                  length) ||
                (enabled != 0 && enabled != 1) || clip.name.empty() ||
                start >= song_step_count || length == 0 ||
                length > song_step_count - start) {
                error = "Invalid visual effect clip in effect preset";
                return false;
            }
            clip.enabled = enabled != 0;
            clip.start_step = static_cast<std::uint16_t>(start);
            clip.length_steps = static_cast<std::uint16_t>(length);
            updated_clips.push_back(std::move(clip));
            continue;
        }
        if (command != "value") {
            error = "Unknown effect preset command: " + command;
            return false;
        }

        std::string uniform;
        float value = 0.0F;
        if (!(input >> uniform >> value)) {
            error = "Invalid value in effect preset";
            return false;
        }
        if (auto* parameter = find_parameter(updated, uniform);
            parameter != nullptr) {
            parameter->value =
                std::clamp(value, parameter->minimum, parameter->maximum);
        }
    }

    if (!reached_end) {
        error = "Effect preset is missing its end marker";
        return false;
    }

    parameters_ = std::move(updated);
    texts_ = std::move(updated_texts);
    clips_ = std::move(updated_clips);
    error.clear();
    return true;
}

void EffectSettings::reset() {
    for (auto& parameter : parameters_) {
        parameter.value = parameter.default_value;
    }
}

std::span<const EffectParameter> EffectSettings::parameters() const {
    return parameters_;
}

std::span<EffectParameter> EffectSettings::parameters() {
    return parameters_;
}

std::span<const TextOverlay> EffectSettings::texts() const {
    return texts_;
}

std::span<TextOverlay> EffectSettings::texts() {
    return texts_;
}

std::size_t EffectSettings::add_text(TextOverlay text) {
    if (text.text.empty()) {
        text.text = "TEXT";
    }
    texts_.push_back(std::move(text));
    return texts_.size() - 1;
}

void EffectSettings::remove_text(std::size_t index) {
    if (index < texts_.size()) {
        texts_.erase(texts_.begin() + static_cast<std::ptrdiff_t>(index));
    }
}

std::span<const EffectClip> EffectSettings::clips() const {
    return clips_;
}

std::span<EffectClip> EffectSettings::clips() {
    return clips_;
}

std::size_t EffectSettings::add_clip(EffectClip clip) {
    clip.start_step = static_cast<std::uint16_t>(
        std::min<std::size_t>(clip.start_step, song_step_count - 1));
    clip.length_steps = static_cast<std::uint16_t>(std::clamp<std::size_t>(
        clip.length_steps, 1, song_step_count - clip.start_step));
    if (clip.name.empty()) {
        clip.name = "STARFIELD";
    }
    clips_.push_back(std::move(clip));
    return clips_.size() - 1;
}

void EffectSettings::remove_clip(std::size_t index) {
    if (index < clips_.size()) {
        clips_.erase(clips_.begin() + static_cast<std::ptrdiff_t>(index));
    }
}

bool EffectSettings::active_at(std::size_t step) const {
    if (clips_.empty()) {
        return true;
    }
    return std::ranges::any_of(clips_, [step](const EffectClip& clip) {
        return clip.enabled && step >= clip.start_step &&
               step < clip.start_step + clip.length_steps;
    });
}

} // namespace tiny
