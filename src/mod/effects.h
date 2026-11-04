#pragma once
// Effects that can be switched off, the HUD, and a dump of the game's classes for working out new features.
#include <string>

namespace effects {

struct Settings {
    bool HideHud = false;
    bool NoHitEffects = false;   // removes the fight's PolarisEffectManager (hit sparks and rage art effects)
    bool NoDramaManager = false; // removes the fight's PolarisDramaManager (camera shakes, rage art drama)
    bool NoBloom = false;
    bool NoMotionBlur = false;
    bool NoDepthOfField = false;
    bool NoParticles = false;
    bool NoVignette = false;
    float DepthDensity = 0.06f;      // how quickly the depth stream goes from black to white with distance
    char CustomCommand[256] = "";
};
Settings& Config();

void OnFightFound();
void OnGameTick();

// Applied on the next game tick.
void RequestRefresh();
void RunCommand(const std::string& command);

// Writes every Polaris* class with its functions and parents to dump_polaris.txt.
void RequestDump();

} // namespace effects
