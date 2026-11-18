#pragma once
// Sharing replays as files. The game keeps every replay decoded in one big block of memory, in fixed slots. Export writes one slot
// to a file. Import puts a file into an empty slot, where the game's replay menu finds it as a downloaded replay, without going
// near Bandai's servers. Nothing is decrypted here and no save file is touched: the game does its own saving, and only saves a
// slot's file when something in that file's group of slots changes through the game's own menus.
#include <cstdint>
#include <string>
#include <vector>

namespace sharing {

struct Slot {
    int Index = 0;
    uint32_t GameVersion = 0; // 30202 is 3.02.02
    uint64_t Played = 0;      // seconds since 1970
    bool Downloaded = false;
    std::string Player1, Player2;
};

// Game thread or menu. Looks the block up the first time, which takes a moment.
std::vector<Slot> Slots();
bool Available();

std::string Folder(); // where shared replay files go
std::vector<std::string> SharedFiles();

// False with the reason in `error`. File names are UTF-8.
bool Export(int slot, const std::string& fileName, std::string& error);
bool Import(const std::string& fileName, std::string& error, bool* alreadyThere = nullptr); // alreadyThere: the same fight is in the list

// One slot, false if it's empty.
bool SlotInfo(int index, Slot& out);
// A file name from the replay's date and players, like "2026-03-23 03-24-33 Jin vs Bryan", with " (2)" and so on added when
// that file is already there.
std::string FileNameFor(const Slot& slot);
// Export under FileNameFor's name, which comes back in `fileName`.
bool ExportNamed(int slot, std::string& fileName, std::string& error);

// Whether a date as the game's replay screens write it ("03/23/2026 03:24:33", in this PC's time zone) is this moment.
bool DateMatches(const std::string& text, uint64_t played);

// Replays from older versions of the game: the game only plays a replay marked with its own version. This marks every older
// replay in memory with the newest version in the list, so the game opens it. The fight is the recorded inputs played through
// the new version's moves, so a move that changed in a patch can make it play out differently from the original match.
// Returns how many it changed (0 if none were older).
int UnlockOlderReplays();

// On by default: a background thread unlocks older replays every few seconds, so they play from the game's own list.
bool& OlderReplaysOn();
void StartOlderReplays();

// On by default: the same thread puts every .t8replay file in the shared folder into the game's Downloaded Replays once the game
// has loaded its own, skipping any that are already in the list. A file added while the game runs is picked up within seconds.
bool& AutoLoadOn();
void AutoLoadTick();
int AutoLoaded(); // how many files it has loaded this session

// Test only: logs every offset in a slot where a 16 or 32 bit value equals `value`.
void FindInSlot(int slot, uint32_t value);

// A replay record somewhere in the game's memory other than its block of slots, such as the one a playing replay was copied into.
struct Copy {
    const void* Address = nullptr;
    size_t RegionSize = 0, RegionOffset = 0;
    uint64_t Played = 0;
    std::string Player1, Player2;
    int Slot = -1; // the slot holding the same replay, -1 if none
};
// Looks through all of the game's writable memory, so it takes a while: never on the game thread in normal use.
std::vector<Copy> Copies();

// What a shared file holds, without importing it.
bool Describe(const std::string& fileName, Slot& out, std::string& error);

} // namespace sharing
