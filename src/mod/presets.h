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

} // namespace presets
