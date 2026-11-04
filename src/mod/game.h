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

// Gameplay time in seconds since the last ZeroClock(). Only moves when the fight moves.
double Clock();
void ZeroClock();
uint64_t StepCount();

// Runs the fight ahead to this point on the clock, as fast as it can. Only goes forward.
void SeekTo(double seconds);
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
double StepAlpha();

struct HookStatus {
    bool Tick = false, Camera = false, Actors = false, Bones = false, GameMode = false, FightStart = false, Fov = false;
};
const HookStatus& Hooks();

// Preview with no game: runs the real tick and camera code each rendered frame, against empty stand-ins.
void PreviewFrame();

} // namespace game
