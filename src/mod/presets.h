#pragma once
// Saving and loading setups. A preset is the look (visuals, effects, lights, recording). A shot is a preset plus the camera
// path and speed ramp, so a friend who has the same replay gets your exact shot.
#include <string>
#include <vector>

namespace presets {

bool SavePreset(const std::string& name);
bool LoadPreset(const std::string& name);
std::vector<std::string> SavedPresets();

bool SaveShot(const std::string& name);
bool LoadShot(const std::string& name);
std::vector<std::string> SavedShots();

// Undo and redo of everything a shot holds: the look, the camera path and the speed ramp. UndoWatch is called by the editor each frame.
void UndoWatch(bool settled);
bool Undo();
bool Redo();

// Look keys on the timeline. A key holds everything the editor can set at one moment, apart from the camera path, the playback speed
// and the recording setup, which have lanes and a tab of their own: the visuals, the lighting rig, the props, effects and actors you
// placed, the attached camera, and ReShade's effects when it's there. Between two keys the sliders, colours, places and sizes move
// smoothly and the switches and choices change at the key. Nothing happens with fewer than two.
std::vector<double> LookKeyTimes();
void AddLookKey(); // at the clock now, replacing a key that's already there
void RemoveLookKey(size_t index);
size_t MoveLookKey(size_t index, double time); // returns where the key ended up
void ClearLookKeys();
// Game thread, once per frame, before the settings are used.
void OnGameTick();

// Every setting of the tool back to how it starts, camera path and speed ramp included. Recording folder and pak mods aren't touched.
void ResetAll();

} // namespace presets
