#pragma once
#include <cstdarg>
#include <filesystem>

namespace mod {

// <game Win64 folder>\tekken8mvm (the launcher creates it; settings, paths and the log live here).
const std::filesystem::path& DataDir();
void SetDataDir(std::filesystem::path path);

void Log(const char* format, ...);

} // namespace mod
