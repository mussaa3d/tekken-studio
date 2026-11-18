#pragma once
// The game's camera, and the studio's own in its place: a free camera you fly, or a dolly path through keys.
#include <cstdint>
#include <string>
#include <vector>

namespace studio::camera {

struct Vec3 {
    float X = 0, Y = 0, Z = 0;
};

// As the game keeps it: where the camera is, the point it looks at, and its field of view in degrees. Y is up, and the units
// are about a millimetre (the fight's own camera stands 4600 back).
struct View {
    Vec3 Eye, Target;
    float Fov = 50;
};

// The record of the camera in use, in the game's memory. 0 when there's none.
uint32_t Record();
bool Read(View& out);
uint64_t Writes(); // how many times the game has written its camera

enum class Mode { Game, Free, Dolly };

// The studio's camera as it's flown: a place, where it points (degrees; yaw 0 looks along -Z, as the fight's camera does at the
// start), and a lens.
struct Pose {
    Vec3 Eye;
    float Yaw = 0, Pitch = 0, Fov = 50;
};
View ToView(const Pose& pose);
Pose ToPose(const View& view);

struct Key {
    double Time = 0; // seconds on the studio's clock
    Pose Pose;
};
enum class PathStyle { Smooth, Linear };

struct Settings {
    Mode Mode = Mode::Game;
    Pose Free;
    float FlySpeed = 3000;   // units a second
    float LookSpeed = 0.15f; // degrees a pixel
    PathStyle Style = PathStyle::Smooth;
    bool ShowPath = true;
};
Settings& Config();

void SetMode(Mode mode);
// The view the game drew last: the studio's when it's on, else the game's own.
bool Current(View& out);
// Puts the studio's view in the game's record, if a studio camera is on. Called right after the game writes its own, and from the
// editor while the fight is held.
void Apply();
// The studio's view for this moment, if a studio camera is on.
bool Wanted(View& out);

std::vector<Key>& Keys();
void AddKeyAtView(double time);
Pose Sample(const std::vector<Key>& keys, double time, PathStyle style);
bool SavePath(const std::string& name);
bool LoadPath(const std::string& name);
std::vector<std::string> SavedPaths();

// A point of the game's world on the picture. x and y run 0 to 1 from the top left. False when it's behind the camera.
bool Project(const View& view, float aspect, Vec3 point, float& x, float& y);

} // namespace studio::camera
