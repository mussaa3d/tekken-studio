#pragma once
// Changing replay from inside a replay. The game has no call for "play this one" that takes a replay, so this walks its own screens with
// key presses, like a player would: to the end of the match, back to the replay list, along to the row, and play.
#include <string>

namespace switcher {

// Goes to this slot's replay. Works from inside a replay or from the replay list. False with the reason in `error`.
bool Start(int slot, std::string& error);
void Cancel();
bool Running();
std::string Status();

// Game thread, once per frame.
void OnGameTick();

} // namespace switcher
