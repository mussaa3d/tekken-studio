#include "mod/sharing.h"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>

#include "mod/log.h"
#include "mod/recorder.h"

namespace sharing {

namespace {

// A slot starts with these. The record size is in the slot itself, so a patch that changes it costs nothing here.
constexpr int HeaderTag = 0;
constexpr int HeaderGameVersion = 8;
constexpr int HeaderPlayed = 16;
constexpr int HeaderDownloaded = 24;
constexpr int HeaderSize = 0x28;
constexpr uint32_t SlotTag = 14;
constexpr int Player1Name = 81434; // from the slot's start
constexpr int Player2Name = 108282;
constexpr int NameBytes = 32;

constexpr uint32_t FileMagic = 0x52433854; // "T8CR"
constexpr uint32_t FileVersion = 1;

struct FileHeader {
    uint32_t Magic = FileMagic;
    uint32_t Version = FileVersion;
    uint32_t RecordSize = 0;
    uint32_t GameVersion = 0;
    uint64_t Stored = 0; // the slot's bytes in the file, after dropping the zeros at the end
};

uint8_t* g_block = nullptr;
size_t g_blockSize = 0;
size_t g_firstSlot = 0;
uint32_t g_slotSize = 0;
std::mutex g_mutex;

uint32_t ReadU32(const uint8_t* at)
{
    uint32_t value = 0;
    std::memcpy(&value, at, sizeof(value));
    return value;
}

uint64_t ReadU64(const uint8_t* at)
{
    uint64_t value = 0;
    std::memcpy(&value, at, sizeof(value));
    return value;
}

bool LooksLikeSlot(const uint8_t* slot, const uint8_t* end)
{
    if (end - slot < 0x100 || ReadU32(slot + HeaderTag) != SlotTag)
        return false;
    const uint32_t size = ReadU32(slot + HeaderSize);
    const uint32_t version = ReadU32(slot + HeaderGameVersion);
    const uint64_t played = ReadU64(slot + HeaderPlayed);
    return size > 0x10000 && size < 0x1000000 && version > 10000 && version < 100000 && played > 1600000000 && played < 2500000000;
}

// Looks through the large blocks of writable memory for one that holds replay slots side by side.
bool FindBlock()
{
    if (g_block)
        return true;
    MEMORY_BASIC_INFORMATION info {};
    for (const uint8_t* address = nullptr; VirtualQuery(address, &info, sizeof(info)) == sizeof(info); address = static_cast<const uint8_t*>(info.BaseAddress) + info.RegionSize) {
        if (info.State != MEM_COMMIT || info.Protect != PAGE_READWRITE || info.RegionSize < (32u << 20) || info.RegionSize > (1ull << 31))
            continue;
        auto* begin = static_cast<uint8_t*>(info.BaseAddress);
        const uint8_t* end = begin + info.RegionSize;
        const uint8_t* scanEnd = begin + std::min<size_t>(info.RegionSize, 8u << 20);
        for (uint8_t* at = begin; at + 0x100 < scanEnd; at += 16) {
            if (!LooksLikeSlot(at, end))
                continue;
            const uint32_t size = ReadU32(at + HeaderSize);
            if (at + static_cast<size_t>(size) * 2 >= end || (!LooksLikeSlot(at + size, end) && ReadU32(at + size) != 0))
                continue;
            // The slots before the first used one are empty.
            size_t first = at - begin;
            first %= size;
            g_block = begin;
            g_blockSize = info.RegionSize;
            g_slotSize = size;
            g_firstSlot = first;
            mod::Log("Replay slots: block at %p, %zu MB, first slot at +0x%zX, %u bytes each", g_block, g_blockSize >> 20, g_firstSlot, g_slotSize);
            return true;
        }
    }
    mod::Log("Replay slots: not found yet");
    return false;
}

int SlotCount()
{
    return g_slotSize ? static_cast<int>((g_blockSize - g_firstSlot) / g_slotSize) : 0;
}

uint8_t* SlotAt(int index)
{
    return g_block + g_firstSlot + static_cast<size_t>(index) * g_slotSize;
}

std::string NameAt(const uint8_t* at)
{
    size_t length = 0;
    while (length < NameBytes && at[length])
        ++length;
    return std::string(reinterpret_cast<const char*>(at), length);
}

Slot Read(int index, const uint8_t* slot)
{
    Slot out;
    out.Index = index;
    out.GameVersion = ReadU32(slot + HeaderGameVersion);
    out.Played = ReadU64(slot + HeaderPlayed);
    out.Downloaded = ReadU64(slot + HeaderDownloaded) != 0;
    out.Player1 = NameAt(slot + Player1Name);
    out.Player2 = NameAt(slot + Player2Name);
    return out;
}

std::filesystem::path PathOf(const std::string& fileName)
{
    return std::filesystem::path(Folder()) / (fileName + ".t8replay");
}

} // namespace

std::vector<Slot> Slots()
{
    std::lock_guard lock(g_mutex);
    std::vector<Slot> slots;
    if (!FindBlock())
        return slots;
    for (int i = 0; i < SlotCount(); ++i)
        if (ReadU32(SlotAt(i) + HeaderTag) == SlotTag)
            slots.push_back(Read(i, SlotAt(i)));
    return slots;
}

bool Available()
{
    std::lock_guard lock(g_mutex);
    return FindBlock();
}

std::string Folder()
{
    const std::filesystem::path folder = recorder::Config().OutputDir / "shared_replays";
    std::error_code ignored;
    std::filesystem::create_directories(folder, ignored);
    return folder.string();
}

std::vector<std::string> SharedFiles()
{
    std::vector<std::string> names;
    std::error_code ignored;
    for (const auto& entry : std::filesystem::directory_iterator(Folder(), ignored))
        if (entry.path().extension() == ".t8replay")
            names.push_back(entry.path().stem().string());
    std::sort(names.begin(), names.end());
    return names;
}

bool Export(int index, const std::string& fileName, std::string& error)
{
    std::lock_guard lock(g_mutex);
    if (!FindBlock()) {
        error = "The game's replays aren't loaded yet";
        return false;
    }
    if (index < 0 || index >= SlotCount() || ReadU32(SlotAt(index) + HeaderTag) != SlotTag) {
        error = "There's no replay in that slot";
        return false;
    }
    const uint8_t* slot = SlotAt(index);
    size_t used = g_slotSize;
    while (used > 0 && slot[used - 1] == 0)
        --used;
    FileHeader header;
    header.RecordSize = g_slotSize;
    header.GameVersion = ReadU32(slot + HeaderGameVersion);
    header.Stored = used;
    std::ofstream file(PathOf(fileName), std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(&header), sizeof(header));
    file.write(reinterpret_cast<const char*>(slot), static_cast<std::streamsize>(used));
    if (!file) {
        error = "Couldn't write the file";
        return false;
    }
    mod::Log("Sharing: exported slot %d to %s, %zu bytes", index, fileName.c_str(), used);
    return true;
}

bool Describe(const std::string& fileName, Slot& out, std::string& error)
{
    std::ifstream file(PathOf(fileName), std::ios::binary);
    FileHeader header;
    file.read(reinterpret_cast<char*>(&header), sizeof(header));
    if (!file || header.Magic != FileMagic || header.Version != FileVersion || header.Stored > header.RecordSize || header.Stored < 0x1000) {
        error = "That isn't a tekken8mvm replay file";
        return false;
    }
    std::vector<uint8_t> data(std::max<size_t>(header.RecordSize, Player2Name + NameBytes));
    file.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(header.Stored));
    if (!file) {
        error = "The file is cut short";
        return false;
    }
    out = Read(-1, data.data());
    return true;
}

bool Import(const std::string& fileName, std::string& error)
{
    std::ifstream file(PathOf(fileName), std::ios::binary);
    FileHeader header;
    file.read(reinterpret_cast<char*>(&header), sizeof(header));
    if (!file || header.Magic != FileMagic || header.Version != FileVersion || header.Stored > header.RecordSize || header.Stored < 0x1000) {
        error = "That isn't a tekken8mvm replay file";
        return false;
    }
    std::lock_guard lock(g_mutex);
    if (!FindBlock()) {
        error = "The game's replays aren't loaded yet";
        return false;
    }
    if (header.RecordSize != g_slotSize) {
        error = "This file is from a different version of the game";
        return false;
    }
    // Replays only play on the game version they were made on.
    uint32_t newest = 0;
    int last = -1;
    for (int i = 0; i < SlotCount(); ++i)
        if (ReadU32(SlotAt(i) + HeaderTag) == SlotTag) {
            newest = std::max(newest, ReadU32(SlotAt(i) + HeaderGameVersion));
            last = i;
        }
    // Right after the last used slot. Further along the block isn't all slots: the end of it holds other things.
    const int empty = last + 1;
    if (newest != 0 && header.GameVersion < newest) {
        error = "This replay is from an older version of the game (" + std::to_string(header.GameVersion) + ", yours is " + std::to_string(newest) + "), so the game wouldn't play it";
        return false;
    }
    if (empty < 0 || empty >= SlotCount() - 8) {
        error = "All the game's replay slots are full";
        return false;
    }

    std::vector<uint8_t> data(g_slotSize);
    file.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(header.Stored));
    if (!file) {
        error = "The file is cut short";
        return false;
    }
    // It shows up as a downloaded replay, dated now in the "when it came in" field.
    if (ReadU64(data.data() + HeaderDownloaded) == 0) {
        const uint64_t now = static_cast<uint64_t>(std::time(nullptr));
        std::memcpy(data.data() + HeaderDownloaded, &now, sizeof(now));
    }
    std::memcpy(SlotAt(empty), data.data(), g_slotSize);
    mod::Log("Sharing: imported %s into slot %d", fileName.c_str(), empty);
    return true;
}

} // namespace sharing
