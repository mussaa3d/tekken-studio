#pragma once
// Changing replay from inside a replay or from the game's replay list. The game has no call for "play this one" that takes a replay, so
// this makes the calls its screens make when clicked (see gameui): to the end of the match, Return to Replay List, the replay's tab,
// the row whose date and players are the replay's, Play Replay, and the side for tips. No keys or clicks, so the game needn't be in front.
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
