#pragma once
// The user's own pak mods (skins, HUDs and so on in the game's Paks folders), switched off and on while the game runs. The game's own
// pak manager mounts and unmounts them, the same code the engine uses for downloaded content. What's already loaded stays as it is until
// the game loads it again, such as on the next replay or fight. A mod switched off stays off at the next start, from the main menu on.
#include <string>
#include <vector>

namespace pakmods {

struct Mod {
    std::string Name;   // the .pak file's name without the extension
    std::string Folder; // where it is, under Paks
    bool Mounted = true;
    bool Busy = false;  // a change is waiting for the game thread
};

// Menu thread. The list is read from the Paks folder the first time.
std::vector<Mod> List();
void Rescan();
void SetMounted(const std::string& name, bool mounted);

// Whether the game's pak manager has been found, and why not.
bool Available();
std::string LastMessage();

// Game thread.
void OnGameTick();

} // namespace pakmods
