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
    // With a fixed shape, the field of view follows it so the fighters stay where the game's 16:9 picture has them: a wider shape
    // shows more at the sides, a portrait one is the middle of the 16:9 view. Off, the shape is a crop of the game's picture.
    bool KeepFighters = true;
    float Fit = 0; // 0 keeps the 16:9 view's height, 1 its width (for portrait: everything the 16:9 picture showed, smaller)
    // The game's HUD with a shape: 0 as the game draws it (over the whole window), 1 in the picture, 2 laid out for the shape.
    int HudFrame = 1;
};
Settings& Config();

void OnFightFound();
void OnGameTick();

// Camera hook, before the game updates the camera.
void BeforeCameraUpdate(void* cameraManager);
// Projection hook: the view the game is about to turn into a projection, so its shape can change. `fov` is the view's own (horizontal,
// degrees) and is widened or narrowed for the shape.
// fromGame: `fov` is the game's own camera's, which the game has already widened for the picture's shape; false for the studio's
// cameras, whose field of view is meant for 16:9.
void AdjustView(void* minimalViewInfo, float& fov, bool fromGame);

// How many lights and meshes the last scan found, for the menu.
struct Counts {
    int CharacterLights = 0;
    int StageLights = 0;
    int PlainMeshes = 0;
};
Counts LastCounts();

} // namespace scene
