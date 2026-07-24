#pragma once

#include "tiny/song.hpp"

#include <filesystem>
#include <string>

namespace tiny {

bool save_song_project(const Song& song, const std::filesystem::path& path,
                       std::string& error);
bool load_song_project(const std::filesystem::path& path, Song& song,
                       std::string& error);

bool export_song_binary(const Song& song, const std::filesystem::path& path,
                        std::string& error);
bool load_song_binary(const std::filesystem::path& path, Song& song,
                      std::string& error);

} // namespace tiny
