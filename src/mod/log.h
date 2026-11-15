#pragma once
#include <cstdarg>
#include <cstring>
#include <filesystem>
#include <string>

namespace mod {

// <game Win64 folder>\tekken8mvm (the launcher creates it; settings, paths and the log live here).
const std::filesystem::path& DataDir();
void SetDataDir(std::filesystem::path path);

void Log(const char* format, ...);

// A plain file name, with no folder in it. Names of presets, shots, paths, takes, replays and sky pictures can come from a file
// someone shared, so they're never allowed to reach another folder or a network share.
inline bool SafeFileName(const std::string& name)
{
    if (name.empty() || name.size() > 200 || name[0] == '.' || name.back() == ' ' || name.back() == '.')
        return false;
    for (const char c : name)
        if (static_cast<unsigned char>(c) < 32 || std::strchr("\\/:*?\"<>|", c))
            return false;
    return true;
}

} // namespace mod
