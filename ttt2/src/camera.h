#pragma once
// The game's camera, and the studio's own in its place.
#include <cstdint>

namespace studio::camera {

struct Vec3 {
    float X = 0, Y = 0, Z = 0;
};

// As the game keeps it: where the camera is, the point it looks at, and its field of view in degrees.
struct View {
    Vec3 Eye, Target;
    float Fov = 50;
};

// The record of the camera in use, in the game's memory. 0 when there's none (the menus).
uint32_t Record();
bool Read(View& out);

// The studio's own camera in place of the game's. While it's on, the record is given the studio's view right after the game
// writes its own, each frame.
bool& Override();
View& Studio();

} // namespace studio::camera
