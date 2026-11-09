#pragma once
// The sun's direction, the sky's rotation, a sky picture of your own (an HDRI) on the stage's sky dome, and hiding a fighter.
#include <filesystem>
#include <string>
#include <vector>

namespace sky {

struct Settings {
    float SunYaw = 0;      // degrees the stage's sun is turned
    float SunHeight = 0;   // degrees it is raised (down for negative)
    float SkyRotation = 0; // degrees the sky is turned
    bool HdriOn = false;
    char HdriFile[260] = ""; // a file in tekken8mvm\hdri, or a full path
    float HdriBrightness = 1;
    bool HdriLightsStage = true; // the stage's sky light takes its light from the new sky
    bool HideFighter[2] = { false, false };
};
Settings& Config();

// Where sky pictures are looked for, and the ones there.
std::filesystem::path Folder();
std::vector<std::string> Files();

// What happened with the sky picture, for the menu. Empty when there's nothing to say.
std::string Status();

void OnFightFound();
// Game thread, once per frame.
void OnGameTick();

} // namespace sky
