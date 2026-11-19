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
void LogStackOnce(); // research: the game's call stack at the next camera write, into the log

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
    // The picture's shape, width over height; 0 is the game's own 16:9. The game always draws 16:9, so a shape is that picture cut
    // to the shape, with the field of view changed first so the fighters stay where 16:9 has them: a wider shape (21:9, 32:9) loses
    // its top and bottom and gets a wider view, so it shows more at the sides at the same size; a portrait shape (9:16, 10:16) is the
    // middle of the picture. Fit, for portrait: 0 keeps the fighters' size, 1 widens the view until the cut holds the whole 16:9 width.
    float ShapeAspect = 0;
    float ShapeFit = 0;
};
Settings& Config();

void SetMode(Mode mode);
// The view the game drew last: the studio's when it's on, else the game's own. Its field of view is the one meant for 16:9.
bool Current(View& out);
// The same with the field of view the picture really has (a shape changes it), for drawing over the picture.
bool Drawn(View& out);
// The field of view that goes to the game for one meant for 16:9, with the shape that's on.
float ShapeFov(float fov);
// The part of a picture of `width` x `height` the shape keeps: its left, top, right and bottom. The whole of it with no shape.
void ShapeRect(float width, float height, float& left, float& top, float& right, float& bottom);
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
