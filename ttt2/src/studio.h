#pragma once
// Tekken Studio for TTT2: what the studio's files share.
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>

namespace rex::ui {
class ImGuiDrawer;
}

namespace studio {

// <the studio exe's folder>\studio: the log, the command channel, saved paths.
const std::filesystem::path& DataDir();
void Log(const char* format, ...);

// The game's memory. The Xbox 360 is big-endian, so every value is swapped on the way in and out.
uint8_t* GuestBase();

inline uint32_t Swap32(uint32_t v) { return _byteswap_ulong(v); }

inline uint32_t ReadU32(uint32_t address)
{
    uint32_t v;
    memcpy(&v, GuestBase() + address, 4);
    return Swap32(v);
}
inline void WriteU32(uint32_t address, uint32_t value)
{
    value = Swap32(value);
    memcpy(GuestBase() + address, &value, 4);
}
inline float ReadFloat(uint32_t address)
{
    const uint32_t v = ReadU32(address);
    float f;
    memcpy(&f, &v, 4);
    return f;
}
inline void WriteFloat(uint32_t address, float value)
{
    uint32_t v;
    memcpy(&v, &value, 4);
    WriteU32(address, v);
}
// Whether these bytes of the game's memory can be read without a fault.
bool Readable(uint32_t address, uint32_t size);

// ----- Time (time.cpp): pause, slow motion and frame steps of an offline fight, through BlueSpark's own hold.
struct TimeControl {
    bool Paused = false;
    double Speed = 1.0; // 0.05 to 1
};
TimeControl& Time();
void StepOneFrame();
// Once for each picture the editor draws: decides whether the fight runs or is held for this one.
void TimeTick();
bool Holding(); // the fight is held right now
bool CanHold(); // a hold would take: BlueSpark's pages are on and an offline fight is up
// The studio's clock in seconds: the replay's own frame count while a replay plays, else the fight frames the studio has let run.
double Clock();
bool ReplayPlaying();

// ----- The editor (editor.cpp)
void CreateEditor(rex::ui::ImGuiDrawer* drawer);
// Simple: only the playback bar, for watching. Studio: the whole editor.
enum class EditorMode { Hidden, Simple, Studio };
EditorMode& Editor();
bool& Markers(); // research: crosses on the fighters, to check the projection

// ----- Research: which code writes or reads a stretch of the game's memory (watch.cpp).
void WatchStart(uint32_t address, uint32_t size, bool reads = false);
void WatchStop();
std::string WatchDump();

// ----- The test channel: commands from studio\commands.txt, answers in studio\state.txt.
void StartCommands();

} // namespace studio
