#pragma once
// The sliders in the Visuals tab: engine settings (cvars) and the fog and lights of the current stage.
// Extra cvar sliders can be added in tekken8mvm\dvars.ini, one per line: Label | r.SomeCvar | min | max | default
#include <string>
#include <vector>

namespace dvars {

enum class Kind {
    Cvar,
    FogDensity,
    FogFalloff,
    FogStartDistance,
    FogMaxOpacity,
    SunIntensity,
    SkyIntensity,
};

struct Slider {
    std::string Group;
    std::string Label;
    Kind Kind = Kind::Cvar;
    std::wstring Cvar;
    float Min = 0, Max = 1, Default = 0;
    float Value = 0;
    bool Overridden = false; // false = the game's own value
    bool Whole = false;      // whole numbers only
    bool Logarithmic = false;
    std::vector<std::string> Choices; // named settings instead of a slider; Value is the index
};

std::vector<Slider>& All();
void LoadExtra(); // reads dvars.ini

void Changed(Slider& slider);
void Reset(Slider& slider);

// Graphics presets for the whole Quality group and the engine's quality sliders, from all low to past the game's own maximum.
constexpr int GraphicsPresetCount = 6;
const char* GraphicsPresetName(int preset); // 0 Low, 1 Medium, 2 High, 3 Epic, 4 Cinematic, 5 Extreme
void ApplyGraphicsPreset(int preset);       // -1 gives every one of them back to the game

struct Colors {
    float Fog[3] = { 0.45f, 0.55f, 0.7f };
    float Sun[3] = { 1, 1, 1 };
    bool FogOverridden = false;
    bool SunOverridden = false;
    bool FogFromCamera = false; // the fog rides along at the camera's height with no falloff, so only distance from the camera thickens it
};
Colors& Tint();
void TintChanged();

void OnFightFound(); // the stage's lights and fog are new objects, so re-apply what's overridden
void OnGameTick();

// Takes over the stage's fog for the greenscreen and depth looks. With the fog pushed out past the fighters, every pixel
// behind them turns solid fog colour. False if this stage has no fog object.
bool SetFogOverride(float r, float g, float b, float density, float startDistance);
void ClearFogOverride();

} // namespace dvars
