#pragma once
// A small lighting rig: your own lights that follow a fighter or stay put.
#include <array>

namespace lights {

enum class Follow { World, P1, P2, Between };

struct Light {
    bool On = false;
    bool Spot = false;
    Follow Follow = Follow::P1;
    float Offset[3] = { 150, -150, 180 };
    float Intensity = 5000;
    float Radius = 1000;
    float Color[3] = { 1, 1, 1 };
};
constexpr int MaxLights = 4;

std::array<Light, MaxLights>& Rig();

void OnFightFound();
void OnGameTick();

} // namespace lights
