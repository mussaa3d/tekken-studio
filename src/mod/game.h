#pragma once
// The game-side hooks: fight clock, time control and smooth motion between 60 Hz gameplay steps.
#include <cstdint>
#include <functional>
#include <vector>

#include "common/cinemath.h"

namespace game {

constexpr double StepSeconds = 1.0 / 60.0;

// Finds the game's code and installs the hooks. Returns false when a required pattern is missing,
// which usually means a game patch moved something; the log names the pattern.
bool Initialize();

// Puts everything back to normal and removes the hooks. Used by the online guard.
void Shutdown();

void RunOnGameThread(std::function<void()> task);

struct TimeControl {
    double Speed = 1.0;
    bool Paused = false;
    bool ClothFollowsFight = true; // jackets, straps and capes run at the fight's speed
    double ClothMinSolve = 1.0 / 480.0; // seconds of fight time in the shortest cloth solve; at 1/960 and under, straps kink and float off the body
    bool BlendAllMeshes = true;    // straps, hair and costumes are physics bones on meshes of their own, and blend between steps as bodies do
    bool ExactFrames = true;       // and every solve the same length, so it lands the same every time
    bool SlowWorldClock = true; // also slow the clock materials can read, which ignores time dilation
    bool NoGameSlowMotion = false; // the game's own slow motion on big hits and KOs stays at normal speed
    bool ScaleEffects = true; // also slow particles, cloth and materials through the world's time dilation
    bool Interpolate = true;  // blend poses between the 60 Hz steps, which is what makes slow motion smooth
    double FpsCap = 0;        // 0 = leave the game's own cap alone
    bool UseSpeedRamp = false; // Speed follows the timeline's timescale keys
};
TimeControl& Time();
void StepOneFrame();

// Timescale keys on the timeline. Safe to call from the menu.
std::vector<cine::SpeedKey> SpeedKeys();
void SetSpeedKeys(std::vector<cine::SpeedKey> keys);

// Where the fight is, in seconds since the last ZeroClock(). This is the timeline: it follows the game's own frame count for the
// match, so it only moves when the fight moves and goes back when a replay goes back.
double Clock();
void ZeroClock();
double ClockZero();
uint64_t StepCount();

// Takes the fight to this point on the clock, as fast as it can. Going backwards needs a replay, with its saved points to jump to;
// anywhere else it only goes forward.
void SeekTo(double seconds);
void StopSeek();
void ReleaseExactFrames();
void SeekToRound(uint32_t frame); // the saved point a round starts from, which is just after its intro
void SeekToStart(); // the very start of the replay, and it plays from there with its intro at normal speed
bool Seeking();

// How fast things are really going, measured over the last second.
struct Rates {
    double TicksPerSecond = 0;  // gameplay steps
    double FramesPerSecond = 0; // rendered frames
};
Rates MeasuredRates();
void CountRenderedFrame();

// While recording, each rendered sample advances the game by exactly speed/samplesPerSecond of game time, however
// long the frame took to render and encode.
void SetRecordingClock(bool enabled, double samplesPerSecond);

// Fighters of the current fight (side 0 = P1, 1 = P2). Null outside a fight.
void* Fighter(int side);
bool FighterLocation(int side, cine::Vec3& out);
int FightCount(); // goes up each time a new fight is found
// The last projection the game asked for: its field of view on the way in and on the way out, and the axis rule it passed.
void LastProjection(float& fovIn, float& fovOut, int& constraint);
int64_t Position(); // the fight's frame count, which is what the clock is made from
// Game thread: every mesh a fighter wears, body first.
std::vector<void*> FighterMeshes(int side);
// Game thread: where the last game step put a fighter, without the blending between steps.
bool FighterPose(int side, cine::Vec3& location, cine::Rot& rotation);
double StepAlpha();

struct HookStatus {
    bool Tick = false, Camera = false, Actors = false, Bones = false, GameMode = false, FightStart = false, Fov = false;
};
const HookStatus& Hooks();

// Preview with no game: runs the real tick and camera code each rendered frame, against empty stand-ins.
void PreviewFrame();

} // namespace game
