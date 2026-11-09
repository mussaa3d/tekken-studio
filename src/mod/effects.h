#pragma once
// Effects that can be switched off, the HUD, and a dump of the game's classes for working out new features.
#include <string>

namespace effects {

struct Settings {
    bool HideHud = false;
    bool KeepBattleStatus = false; // with the HUD hidden, leaves the combo, damage, counter, clean, punish and tornado text
    bool HideHitLabels = false; // the words that pop up on hits (Counter and so on) and the frame data panel, which are one widget; the effects and the rest of the HUD stay
    char HitLabelWidgets[160] = "practice_info"; // the widget classes those words are, comma separated, matched against the class name
    bool NoHitEffects = false;   // hides hit sparks and the glows of rage arts and heat
    bool FogHidesMist = true;    // choosing a fog colour also hides the map's own mist and drifting particles
    bool NoDramaManager = false; // cancels the screen effects a rage art or heat smash puts over the picture
    bool NoBloom = false;
    bool NoMotionBlur = false;
    bool NoDepthOfField = false;
    bool NoParticles = false;
    bool NoVignette = false;
    char CustomCommand[256] = "";
};
Settings& Config();

void OnFightFound();
void OnGameTick();
bool DramaPlaying(); // a rage art or heat smash cinematic is running

// Game thread: the actor that owns a component, and whether it's part of a fighter (their meshes, props and effects).
void* OwnerOf(void* component);
bool IsFighterPart(void* owner);

// Test only, game thread: writes the class of every widget on screen to the log.
void LogVisibleWidgets();

// Applied on the next game tick.
void RequestRefresh();
void RunCommand(const std::string& command);

// Writes every Polaris* class with its functions and parents to dump_polaris.txt.
void RequestDump();

} // namespace effects
