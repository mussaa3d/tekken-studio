#pragma once
// The game's own replay screens, driven by the calls their Blueprints make into the game's code when a player clicks, instead of
// key presses: the replay list, the details box a row opens, and the game's question boxes (the side for tips, the end of a
// replay). It follows what the game tells those screens (each row's date and players, each button's text) through engine events,
// which also says which saved replay is open. And it turns one of the details box's greyed-out buttons into Export Replay.
#include <string>
#include <vector>

namespace gameui {

void Install(); // once the engine is found
void OnGameTick();

// What's up, from the game's own events. Game thread.
bool ListUp();     // the replay list, with no box over it
bool DetailsUp();  // a row's details box
bool QuestionUp(); // one of the game's question boxes
int ListTab();     // the list's tab: see the constants below, -1 if not known
int ListRows();    // rows the game has given the list since it last emptied it
int ListFills();   // goes up each time the game empties the list to fill it again
int QuestionsShown(); // goes up each time a question box opens
int RowOfSlot(int slot); // the list's row showing that slot, -1 if none does
int DetailsSlot();       // the slot the details box is about, -1 if none or not a saved replay
bool DetailsShow(int slot); // the details box is about that replay (or an exact copy of it in another slot)
std::vector<std::string> QuestionButtons(); // the question box's buttons, as the game passed them

constexpr int TabOnline = 0, TabDownloaded = 2, TabMine = 3;

// The calls a click makes. False if the widget isn't there. Game thread.
bool OpenTab(int tab);
bool OpenRow(int row);
// The boxes take the button their cursor is on, whatever button a decide names, so the cursor is moved there first (as hovering
// does) and a decide waits until the box's cursor says it's there.
bool SelectDetails(int id);
bool SelectQuestion(int id);
int DetailsCursor();
int QuestionCursor();
bool DecideDetails(int id); // 0 is Play Replay
bool DecideQuestion(int id);

// The saved replay that's playing, -1 if none is or it isn't a saved one (an online replay). Any thread.
int OpenSlot();

// The Export Replay button in the game's details box, on by default.
bool& ExportButtonOn();

} // namespace gameui
