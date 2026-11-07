#pragma once
// The stage and its lighting, after TekkenOverlay's display settings and performance stages: switching the characters' lights and the
// stage's lights off, tinting the characters' light, the stage drawn in one plain material, no camera shake, and the shape of the picture.
namespace scene {

enum class Frame { Game, Fill, Fixed };

struct Settings {
    bool NoCharacterLights = false; // the lights that only light the fighters
    bool NoStageLights = false;     // every other light, sun included
    bool TintCharacterLights = false;
    float CharacterLightColor[3] = { 1.0f, 0.85f, 0.7f };
    bool PlainStage = false; // the stage's meshes all get the engine's plain grey material
    bool NoCameraShake = false;
    Frame Frame = Frame::Game;
    float AspectRatio = 2.39f; // width over height, for Frame::Fixed
};
Settings& Config();

void OnFightFound();
void OnGameTick();

// Camera hook, before the game updates the camera.
void BeforeCameraUpdate(void* cameraManager);
// Projection hook: the view the game is about to turn into a projection, so its shape can change.
void AdjustView(void* minimalViewInfo);

// How many lights and meshes the last scan found, for the menu.
struct Counts {
    int CharacterLights = 0;
    int StageLights = 0;
    int PlainMeshes = 0;
};
Counts LastCounts();

} // namespace scene
