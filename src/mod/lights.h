#pragma once
// A lighting rig: your own lights that follow a fighter or stay where you put them.
#include <array>

namespace lights {

enum class Follow { World, P1, P2, Between };
// What a spot light points at: its own direction, or a fighter.
enum class Aim { Direction, P1, P2, Between };

struct Light {
    bool On = false;
    bool Spot = false;
    Follow Follow = Follow::P1;
    float Offset[3] = { 150, -150, 180 }; // from what it follows, or the place in the world when it follows nothing
    float Intensity = 50000000;
    float Radius = 1000;
    float Color[3] = { 1, 1, 1 };
    Aim Aim = Aim::Direction;
    float Direction[2] = { -30, 0 }; // pitch and yaw of a spot light
    float Cone = 40;                 // a spot light's outer cone, degrees from its centre
    bool Shadows = true;
};
constexpr int MaxLights = 8;

std::array<Light, MaxLights>& Rig();

// Puts a light where the camera is, looking the way it looks.
void PlaceAtCamera(int index);

void OnFightFound();
void OnGameTick();
bool IsRigLight(const void* component); // one of the rig's own lights, which the stage's light switches leave alone

} // namespace lights
