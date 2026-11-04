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

// False with the reason in `error`.
bool Export(int slot, const std::string& fileName, std::string& error);
bool Import(const std::string& fileName, std::string& error);

// What a shared file holds, without importing it.
bool Describe(const std::string& fileName, Slot& out, std::string& error);

} // namespace sharing
