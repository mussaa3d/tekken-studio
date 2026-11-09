#pragma once
// The replay's on-screen panels, switched by calling the game's own widget functions.

namespace replaydisplay {

constexpr int Rows = 10;
const char* Label(int row);

// -1 leaves the row to the game, 0 hides it, 1 shows it.
int& Choice(int row);
void Changed();
void ClearChoices();

void OnFightFound();

// Game thread, once per frame.
void OnGameTick();

} // namespace replaydisplay
