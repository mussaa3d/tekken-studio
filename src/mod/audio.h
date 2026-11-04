#pragma once
// The audio pass. Frame-locked video has no usable sound, so the same part of the replay is played once more at normal
// speed while only this game's sound is captured to a WAV file. Windows 10 2004 or newer.
#include <filesystem>
#include <string>

namespace audio {

struct Settings {
    bool Armed = false; // captures on its own across the timeline's range
};
Settings& Config();

bool Start(const std::filesystem::path& file);
void Stop();
bool Running();
double SecondsCaptured();
bool HeardSound(); // anything louder than silence so far
std::string LastFile();
std::string LastError();

void OnGameTick(double clockSeconds);

} // namespace audio
