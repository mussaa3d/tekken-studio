#pragma once
// Where the fight really is, and moving it there. The game's battle system counts frames across the whole match, and when a replay
// opens the game runs through it once and keeps a list of saved points: one every five seconds or so, one at each round start. Its
// own rewind button restores one of those. Restoring the one we pick gives the timeline a real rewind: land on the last saved
// point before a moment, then run forward to the exact frame.
#include <cstdint>
#include <vector>

namespace replay {

struct Code {
    uintptr_t InputHandler = 0; // the replay controller's per-frame input code, where the game's own rewind runs
    uintptr_t RewindSite = 0;   // the spot in it that calls the rewind
    uintptr_t RoundRead = 0;    // code that reads the round number
};

// Any of the addresses can be zero when a patch moved them: the timeline then counts steps and rewinding is off.
void Setup(const Code& code);

// The frame since the match began, and the round. False outside a fight.
bool Position(int& round, uint32_t& frame);

// A replay is open, with saved points to jump to.
bool CanJump();

// Done on the next pass through the replay's input code. Landing is on a saved point far enough before the frame to run up to it
// exactly. With roundStart it lands on the round start at the frame itself, and the game's intro for that round then moves it on a little.
void RequestJump(uint32_t frame, bool roundStart = false);
constexpr int SeekRunUp = 60; // steps a seek plays at normal pace before landing
bool JumpPending();
bool JumpSettling(); // the game's short transition after a jump is still running
void CancelJump();

struct SavedPoint {
    uint32_t Frame = 0;
    int Round = 0;
    int Type = 0; // 3 = a round starts, 2 = the end of the match
};
std::vector<SavedPoint> SavedPoints(); // safe from the menu
constexpr int TypeRoundStart = 3;
constexpr int TypeMatchEnd = 2;

} // namespace replay
