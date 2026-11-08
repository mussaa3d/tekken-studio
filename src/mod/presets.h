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

// Every setting of the tool back to how it starts, camera path and speed ramp included. Recording folder and pak mods aren't touched.
void ResetAll();

} // namespace presets
