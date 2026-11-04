#pragma once
// Which camera the player sees: the game's own, a free-flying one, a keyframed dolly path, or one attached to a fighter.
// Also the filter that keeps the normal fight camera through rage arts and heat smashes.
#include <string>
#include <vector>

#include "common/cinemath.h"

namespace camera {

enum class Mode { Game, Free, Dolly, Attach };

// Off = untouched. Cinematics = hold the normal framing only while the game cuts to a cinematic.
// Classic = always the fixed side-on framing, whatever the game does.
enum class Filter { Off, Cinematics, Classic };

struct Settings {
    Mode Mode = Mode::Game;
    Filter Filter = Filter::Off;
    double FlySpeed = 300;    // cm per real second
    double LookSpeed = 0.12;  // degrees per mouse pixel at 45 degrees FOV
    double Fov = 45;
    double Roll = 0;
    bool FovOverride = false; // use Fov with the game's camera too
    double JumpDistance = 60; // how far the game's camera may leave its normal spot before it counts as a cut
    double JumpAngle = 8;
    double SeparationZoom = 0.15; // how far the held camera backs off per cm the fighters move apart
    double NormalFov = 65;       // the game's lens during normal fighting
    bool PathOnRealTime = false; // true: the dolly plays in real seconds even while the game is paused
    bool LinearPath = false;     // straight lines between keys instead of a smooth curve
};
Settings& Config();

// Fixed = the camera moves and turns with the bone. Orbit = circles the bone, right mouse to turn and wheel for distance.
// Watch = flies like the free camera, always aimed at the bone.
enum class AttachStyle { Fixed, Orbit, Watch };

struct AttachSettings {
    int Fighter = 0;
    char Bone[64] = "";
    AttachStyle Style = AttachStyle::Orbit;
    float Offset[3] = { 40, 0, 0 };   // along the bone's axes, for Fixed
    float Turn[3] = { 0, 180, 0 };    // pitch, yaw, roll on top of the bone's, for Fixed
    float Smooth = 0.5f;              // 0 = rigid, near 1 = floats behind the bone
    float OrbitDistance = 150;
    float OrbitYaw = 0;
    float OrbitPitch = 0;
};
AttachSettings& Attach();
std::vector<std::string> Bones(int fighter); // bone names, once a fight is on

// The game takes its field of view from the view it hands the projection function, not from the camera cache. This reports
// what the game wanted there and returns what to use.
float ProjectionFov(float gameFov);

// Game thread, once per rendered frame, just after the engine updated its camera.
void OnCameraUpdate(void* cameraManager);
void MarkNewStep();
void OnFightFound();
void OnFightJump(); // a replay jumped: the camera mustn't blend or read a cut across it

void ToggleFree();
void ToggleDolly();
void SetFreePose(cine::Vec3 location, cine::Rot rotation, double fov);
bool WantsKeyboard(); // the free camera is flying, so WASD should not reach the game

struct View {
    cine::Vec3 Location;
    cine::Rot Rotation;
    double Fov = 45;
    bool Valid = false;
};
View LastView();
bool InCinematicHold();

// Dolly path. All of these are safe to call from the menu (render thread).
std::vector<cine::CameraKey> Keys();
void SetKeys(std::vector<cine::CameraKey> keys);
void AddKeyAtCurrentView();
void RemoveKey(size_t index);
void ClearKeys();
void GoToKey(size_t index);
size_t MoveKey(size_t index, double time); // returns where the key ended up after re-sorting
bool SavePath(const std::string& name);
bool LoadPath(const std::string& name);
std::vector<std::string> SavedPaths();
double PathClock();
void RestartPathClock();

// The whole cinematic from its first key, right now: goes there, flies the path and stops on the last key.
void PlayShot();
bool PlayingShot();

// Writes the path as an HLAE CamIO file so it can be taken into After Effects or Blender.
bool ExportCamIo(const std::string& name, double fps);

} // namespace camera
