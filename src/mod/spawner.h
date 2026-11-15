#pragma once
// Things you put on the stage yourself: props (any mesh the game has) and effects (any particle system the game has). Each has a place,
// a turn and a size, which the look keys can move, and an effect can run off the fight clock so it's the same on every pass of a recording.
#include <array>
#include <string>
#include <vector>

namespace spawner {

enum class Follow { World, P1, P2, Between };

struct Prop {
    bool On = false;
    char Asset[200] = "";   // the name of a mesh that's loaded, or a full path the game can load
    int Follow = 0;         // a Follow, kept as a number for the menu
    float Location[3] = {}; // in the world, or from what it follows
    float Rotation[3] = {}; // pitch, yaw, roll
    float Scale[3] = { 1, 1, 1 };
    bool Shadows = true;
};
constexpr int MaxProps = 12;

struct Effect {
    bool On = false;
    char Asset[200] = "";
    int Follow = 0;
    float Location[3] = {};
    float Rotation[3] = {};
    float Scale = 1;
    bool OnTimeline = true; // its age comes from the fight clock, counted from Start
    double Start = 0;       // seconds on the fight clock
    float Length = 0;       // seconds it shows for; 0 = from Start on
    float Rate = 1;         // how fast it ages against the fight clock
};
constexpr int MaxEffects = 8;

std::array<Prop, MaxProps>& Props();
std::array<Effect, MaxEffects>& Effects();

// Puts a prop or an effect a little in front of the camera, and switches it on.
void PlacePropAtCamera(int index);
void PlaceEffectAtCamera(int index);
// Starts an effect that isn't on the timeline over again.
void RestartEffect(int index);

// What the game has loaded right now that can be placed. The lists are made on the game thread after AskForLists.
void AskForLists();
std::vector<std::string> MeshNames();
std::vector<std::string> EffectNames();

// What happened with a slot, for the menu. Empty when it's simply there.
std::string PropStatus(int index);
std::string EffectStatus(int index);

// For the recording looks: a placed effect counts as an effect, and a prop as part of the stage.
bool IsPlacedEffect(const void* component);

void OnFightFound();
void OnGameTick();

} // namespace spawner
