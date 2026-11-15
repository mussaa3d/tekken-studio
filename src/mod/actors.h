#pragma once
// Actors: copies of a fighter that you place on the stage, playing motion recorded from a replay.
//
// A take is a fighter's motion over a stretch of a replay: every bone of every mesh they wear, 60 times a second, and where they stood.
// An actor is a copy of a fighter's meshes that plays a take from a moment on the timeline, somewhere you choose. Its pose comes from the
// fight clock alone, so scrubbing, rewinding and recording passes all show it in the same pose at the same moment.
#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "common/cinemath.h"

namespace actors {

struct Actor {
    bool On = false;
    char Take[64] = "";
    int Body = -1;          // whose meshes it wears: -1 the fighter the take was made from, 0 P1, 1 P2
    double Start = 0;       // seconds on the fight clock where the take begins
    float Speed = 1;
    int End = 0;            // a cine::TakeEnd: hold the last pose, loop, or disappear
    float Location[3] = {}; // where the take's first frame stands
    float Rotation[3] = {}; // pitch, yaw, roll of the take's first frame
    float Scale = 1;
};
constexpr int MaxActors = 6;
std::array<Actor, MaxActors>& Cast();

struct TakeInfo {
    std::string Name;
    int Frames = 0;
    int Side = 0;
    int Meshes = 0;
};
// Takes in memory and in the takes folder. Safe from the menu.
std::vector<TakeInfo> Takes();
void RefreshTakes();

// Records a take of one fighter from `in` to `out` on the fight clock: goes there, plays it at normal speed and stops at the end.
bool StartTake(const std::string& name, int side, double in, double out, std::string& error);
void CancelTake();
bool RecordingTake();
std::string TakeStatus(); // what the take recorder is doing or last did
void DeleteTake(const std::string& name);

// Gives an actor a take, and puts it where the fighter stood when the take began.
void SetTake(int index, const std::string& name);
// Moves an actor to in front of the camera, at the height it was recorded at.
void PlaceAtCamera(int index);
std::string ActorStatus(int index);

// For the recording looks: an actor counts as a fighter.
bool IsActorPart(const void* actor);

// From the game's hooks.
// Bone hook, any thread: fills in the pose of an actor's mesh. True if the component was one.
bool DrivePose(void* component, void* transforms, int count);
// Bone hook, any thread: a fighter's mesh as the last game step left it, while a take is being recorded.
void CaptureBones(void* component, const void* transforms, int count);

void OnFightFound();
void OnGameTick();

} // namespace actors
