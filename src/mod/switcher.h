#pragma once
// Changing replay from inside a replay. The game has no call for "play this one" that takes a replay, so this walks its own screens with
// key presses, like a player would: to the end of the match, back to the replay list, along to the row, and play.
#include <string>

namespace switcher {

// Goes to this slot's replay. Works from inside a replay or from the replay list. False with the reason in `error`.
bool Start(int slot, std::string& error);
void Cancel();

// The replay's own display settings (its menu under B). A choice of -1 leaves that row alone.
constexpr int DisplayRows = 15;
struct DisplayRow {
    const char* Label;
    int Options; // how many choices the game's list for this row has
};
const DisplayRow& DisplayRowInfo(int row);
const char* DisplayOption(int row, int option);
bool ApplyDisplay(const int (&choices)[DisplayRows], std::string& error);
bool Running();
std::string Status();

// Game thread, once per frame.
void OnGameTick();

} // namespace switcher
