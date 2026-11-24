#pragma once
// Tekken Studio for TTT2: what the studio's files share.
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

// A path as UTF-8 text, for the log and the editor. (path::string() throws for a name the PC's code page doesn't have.)
inline std::string Utf8(const std::filesystem::path& path)
{
    const std::u8string text = path.u8string();
    return std::string(text.begin(), text.end());
}
// A path as BlueSpark takes one: the code page's text, or UTF-8 where the code page can't say it.
inline std::string Narrow(const std::filesystem::path& path)
{
    try {
        return path.string();
    } catch (const std::exception&) {
        return Utf8(path);
    }
}

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
// While a BlueSpark replay plays, whether the game may run this frame (skip.cpp asks once a frame, on the game's thread): BlueSpark's
// hold doesn't reach a replay, so the studio leaves the game's frame out instead. Always true outside a replay.
bool ReplayFrameAllowed();
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

// Research (skip.cpp): one of the game's frame functions left out while `hold`; calls and skips of each, for the state file.
// The game's sound to a WAV file (audio.cpp).
namespace audio {
bool Start(const std::filesystem::path& file);
void Stop();
bool Running();
std::string LastError();
} // namespace audio

// The game's HUD in presets (hud.cpp): 0 the game's own, 1 clean (health bars, timer and portraits), 2 health bars and timer,
// 3 health bars, 4 timer, 5 none.
constexpr int HudPresetCount = 6;
const char* HudPresetName(int preset);
int& HudPreset();
void HudApplyPreset(int preset);

// Replays kept and passed round (replays.cpp). A copy of every replay BlueSpark writes goes to Videos\tekken8mvm\ttt2_replays
// while KeepsReplays (on by default, remembered); a replay file from anywhere is copied into BlueSpark's folder to be watched.
void StartReplayKeeper();
const std::filesystem::path& SavedReplaysFolder();
bool KeepsReplays();
void KeepReplays(bool on);
int ReplaysKept();
int ReplaysBroughtIn();
bool SaveReplayCopy(const std::filesystem::path& file);
bool ReplayIsSaved(const std::filesystem::path& file);
std::filesystem::path BringReplayIn(const std::filesystem::path& file);
std::vector<std::filesystem::path> SavedReplaysNotIn();
void PickReplaysToBringIn();

// Recording with sound runs in real time; without, frame for frame. On by default.
bool& RecorderWithSound();
// A recording that paces the game itself, frame by frame (the kind without sound).
bool RecorderPacesGame();

// Recording to video, frame for frame (recorder.cpp). Start and Stop from the overlay's thread; AllowsFrame is asked by skip.cpp once a
// game frame while a recording runs; OnPresent once a present.
bool RecorderActive();
void RecorderStart();
void RecorderStop();
bool RecorderAllowsFrame();
void RecorderOnPresent();
uint64_t RecorderFrames();
std::string RecorderMessage(); // "Recording x.mp4", "Saved x.mp4", or what went wrong
std::filesystem::path RecorderFolder();

// BlueSpark's own overlay (hints, pill, pages) taken off the presenter, or put back (overlay.cpp).
void HideBlueSparkOverlay(bool hide);
void DumpFrameOnce(); // research: logs the ImGui windows in the next frame
// The game's HUD (hud.cpp): each place that asks "is the mode 7" can be answered yes, which hides that part of the HUD.
int HudCallerCount();
void HudHideCaller(int index, bool hide);
void HudHideNone();
std::string HudCallersSeen(); // research: the callers that asked since the last look
// The game's HUD fitted into the picture's shape (hudshape.cpp). On by default.
bool& HudFollowsShape();
uint64_t HudPassesSeen();
void SkipTest(int which, bool hold);
std::string SkipCounts();

} // namespace studio
