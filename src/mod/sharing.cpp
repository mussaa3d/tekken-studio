#include "mod/sharing.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <tuple>

#include "mod/log.h"

namespace sharing {

namespace {

// A slot starts with these. The record size is in the slot itself, so a patch that changes it costs nothing here.
constexpr int HeaderTag = 0;
constexpr int HeaderGameVersion = 8;
constexpr int HeaderPlayVersion = 12; // the same version again, which the game checks before playing
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

// A replay file's header, checked before anything in it is trusted: these files are passed between people.
bool HeaderOk(const FileHeader& header)
{
    return header.Magic == FileMagic && header.Version == FileVersion && header.RecordSize > 0x10000 && header.RecordSize < 0x1000000
        && header.RecordSize >= static_cast<uint32_t>(Player2Name + NameBytes) && header.Stored >= 0x1000 && header.Stored <= header.RecordSize;
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
    static bool logged = false;
    if (!logged)
        mod::Log("Replay slots: not found yet");
    logged = true;
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

// UTF-8 both ways: player names are often not in the PC's code page, and a std::string path would be read in that code page.
std::filesystem::path Utf8Path(const std::string& text)
{
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(text.data()), text.size()));
}

std::string Utf8Of(const std::filesystem::path& path)
{
    const std::u8string text = path.u8string();
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

std::filesystem::path (*g_outputDirSource)() = nullptr;

std::filesystem::path OutputDir()
{
    if (g_outputDirSource)
        return g_outputDirSource();
    wchar_t profile[MAX_PATH] = L".";
    GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH);
    return std::filesystem::path(profile) / "Videos" / "tekken8mvm";
}

std::filesystem::path FolderPath()
{
    const std::filesystem::path folder = OutputDir() / "shared_replays";
    std::error_code ignored;
    std::filesystem::create_directories(folder, ignored);
    return folder;
}

std::filesystem::path PathOf(const std::string& fileName)
{
    return FolderPath() / Utf8Path(fileName + ".t8replay");
}

// Where your own replays are exported to on their own: apart from the shared folder, so they aren't loaded back into the game.
std::filesystem::path MyFolderPath()
{
    const std::filesystem::path folder = OutputDir() / "my_replays";
    std::error_code ignored;
    std::filesystem::create_directories(folder, ignored);
    return folder;
}

// A file name from the replay's date and players in `folder`, with " (2)" and so on when it's taken.
std::string NameIn(const std::filesystem::path& folder, const Slot& slot)
{
    const time_t played = static_cast<time_t>(slot.Played);
    tm local {};
    localtime_s(&local, &played);
    char date[32];
    strftime(date, sizeof(date), "%Y-%m-%d %H-%M-%S", &local);
    std::string name = std::string(date) + " " + slot.Player1 + " vs " + slot.Player2;
    for (char& c : name)
        if (static_cast<unsigned char>(c) < 32 || std::strchr("\\/:*?\"<>|", c))
            c = '_';
    while (!name.empty() && (name.back() == ' ' || name.back() == '.'))
        name.pop_back();
    std::error_code ignored;
    std::string unique = name;
    for (int n = 2; std::filesystem::exists(folder / Utf8Path(unique + ".t8replay"), ignored) && n < 1000; ++n)
        unique = name + " (" + std::to_string(n) + ")";
    return unique;
}

// The versions a replay had before it was marked with the newest one so the game plays it (UnlockOlderReplays, Import), by fight,
// so a file written from it says the version it was really played on. Under g_mutex.
using Fight = std::tuple<uint64_t, std::string, std::string>;
std::map<Fight, std::pair<uint32_t, uint32_t>> g_realVersions;

Fight FightOf(const uint8_t* slot)
{
    const Slot info = Read(-1, slot);
    return { info.Played, info.Player1, info.Player2 };
}

// Writes one slot to `path`. The caller holds g_mutex and has found the block.
bool WriteSlot(int index, const std::filesystem::path& path, std::string& error)
{
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
    // The slot's start, with the version it was really played on if it was marked newer to play here.
    uint8_t start[HeaderPlayVersion + 4];
    std::memcpy(start, slot, sizeof(start));
    const auto real = g_realVersions.find(FightOf(slot));
    if (real != g_realVersions.end()) {
        header.GameVersion = real->second.first;
        std::memcpy(start + HeaderGameVersion, &real->second.first, 4);
        std::memcpy(start + HeaderPlayVersion, &real->second.second, 4);
    }
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(&header), sizeof(header));
    file.write(reinterpret_cast<const char*>(start), static_cast<std::streamsize>(std::min(used, sizeof(start))));
    if (used > sizeof(start))
        file.write(reinterpret_cast<const char*>(slot + sizeof(start)), static_cast<std::streamsize>(used - sizeof(start)));
    if (!file) {
        error = "Couldn't write the file";
        return false;
    }
    mod::Log("Sharing: exported slot %d to %s, %zu bytes", index, Utf8Of(path).c_str(), used);
    return true;
}

// What the file at `path` holds.
bool DescribePath(const std::filesystem::path& path, Slot& out, std::string& error)
{
    std::ifstream file(path, std::ios::binary);
    FileHeader header;
    file.read(reinterpret_cast<char*>(&header), sizeof(header));
    if (!file || !HeaderOk(header)) {
        error = "That isn't a tekken8mvm replay file";
        return false;
    }
    std::vector<uint8_t> data(std::max<size_t>(header.RecordSize, Player2Name + NameBytes));
    file.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(std::min<uint64_t>(header.Stored, data.size())));
    if (!file) {
        error = "The file is cut short";
        return false;
    }
    out = Read(-1, data.data());
    return true;
}

} // namespace

void SetOutputDirSource(std::filesystem::path (*source)())
{
    g_outputDirSource = source;
}

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
    return FolderPath().string();
}

std::vector<std::string> SharedFiles()
{
    std::vector<std::string> names;
    std::error_code ignored;
    for (const auto& entry : std::filesystem::directory_iterator(FolderPath(), ignored))
        if (entry.path().extension() == ".t8replay")
            names.push_back(Utf8Of(entry.path().stem()));
    std::sort(names.begin(), names.end());
    return names;
}

bool SlotInfo(int index, Slot& out)
{
    std::lock_guard lock(g_mutex);
    if (!FindBlock() || index < 0 || index >= SlotCount() || ReadU32(SlotAt(index) + HeaderTag) != SlotTag)
        return false;
    out = Read(index, SlotAt(index));
    return true;
}

std::string FileNameFor(const Slot& slot)
{
    return NameIn(FolderPath(), slot);
}

bool ExportNamed(int index, std::string& fileName, std::string& error)
{
    Slot slot;
    if (!SlotInfo(index, slot)) {
        error = "There's no replay in that slot";
        return false;
    }
    fileName = FileNameFor(slot);
    return Export(index, fileName, error);
}

bool DateMatches(const std::string& text, uint64_t played)
{
    // The numbers in the text, in order, and whether it's a 12-hour clock.
    int numbers[8] = {};
    int count = 0;
    for (size_t i = 0; i < text.size() && count < 8;) {
        if (text[i] < '0' || text[i] > '9') {
            ++i;
            continue;
        }
        int value = 0;
        for (; i < text.size() && text[i] >= '0' && text[i] <= '9'; ++i)
            value = value * 10 + (text[i] - '0');
        numbers[count++] = value;
    }
    if (count != 6)
        return false;
    const bool pm = text.find("PM") != std::string::npos || text.find("pm") != std::string::npos;
    const bool am = text.find("AM") != std::string::npos || text.find("am") != std::string::npos;
    const time_t t = static_cast<time_t>(played);
    tm local {};
    if (localtime_s(&local, &t) != 0)
        return false;
    int hour = local.tm_hour;
    if (pm || am)
        hour = hour % 12 == 0 ? 12 : hour % 12;
    if (numbers[3] != hour || numbers[4] != local.tm_min || numbers[5] != local.tm_sec)
        return false;
    // The date can be written in any order, with a short or a long year.
    int date[3] = { numbers[0], numbers[1], numbers[2] };
    const int year = local.tm_year + 1900;
    const bool longYear = date[0] >= 100 || date[1] >= 100 || date[2] >= 100;
    std::sort(std::begin(date), std::end(date));
    int wanted[3] = { longYear ? year : year % 100, local.tm_mon + 1, local.tm_mday };
    std::sort(std::begin(wanted), std::end(wanted));
    return std::equal(std::begin(date), std::end(date), std::begin(wanted));
}

bool Export(int index, const std::string& fileName, std::string& error)
{
    if (!mod::SafeFileName(fileName)) {
        error = "Use a name without \\ / : * ? \" < > |";
        return false;
    }
    std::lock_guard lock(g_mutex);
    if (!FindBlock()) {
        error = "The game's replays aren't loaded yet";
        return false;
    }
    return WriteSlot(index, PathOf(fileName), error);
}

bool Describe(const std::string& fileName, Slot& out, std::string& error)
{
    if (!mod::SafeFileName(fileName)) {
        error = "That isn't a tekken8mvm replay file";
        return false;
    }
    return DescribePath(PathOf(fileName), out, error);
}

// The newest version any replay has had, kept between sessions: with only older replays in the list, the list alone can't tell.
uint32_t RememberedVersion()
{
    std::ifstream file(mod::DataDir() / "replay_version.txt");
    uint32_t version = 0;
    file >> version;
    return version >= 10000 && version < 100000 ? version : 0;
}

uint32_t NewestVersion()
{
    uint32_t newest = 0;
    for (int i = 0; i < SlotCount(); ++i)
        if (ReadU32(SlotAt(i) + HeaderTag) == SlotTag) {
            const uint32_t version = ReadU32(SlotAt(i) + HeaderGameVersion);
            if (version < 100000)
                newest = std::max(newest, version);
        }
    static uint32_t remembered = RememberedVersion();
    if (newest > remembered) {
        remembered = newest;
        std::ofstream(mod::DataDir() / "replay_version.txt") << newest;
    }
    return std::max(newest, remembered);
}

int UnlockOlderReplays()
{
    std::lock_guard lock(g_mutex);
    if (!FindBlock())
        return 0;
    const uint32_t newest = NewestVersion();
    int changed = 0;
    for (int i = 0; i < SlotCount(); ++i) {
        uint8_t* slot = SlotAt(i);
        if (ReadU32(slot + HeaderTag) != SlotTag)
            continue;
        const uint32_t version = ReadU32(slot + HeaderGameVersion);
        if (version >= newest || version < 10000)
            continue;
        g_realVersions.emplace(FightOf(slot), std::make_pair(version, ReadU32(slot + HeaderPlayVersion)));
        // The version is in the slot twice; the game checks the second one before it plays.
        std::memcpy(slot + HeaderGameVersion, &newest, sizeof(newest));
        const uint32_t second = ReadU32(slot + HeaderPlayVersion);
        if (second >= 10000 && second < 100000)
            std::memcpy(slot + HeaderPlayVersion, &newest, sizeof(newest));
        ++changed;
    }
    if (changed)
        mod::Log("Sharing: %d older replays marked as version %u", changed, newest);
    return changed;
}

void FindInSlot(int index, uint32_t value)
{
    std::lock_guard lock(g_mutex);
    if (!FindBlock() || index < 0 || index >= SlotCount())
        return;
    const uint8_t* slot = SlotAt(index);
    std::string found;
    int count = 0;
    for (uint32_t at = 0; at + 4 <= g_slotSize && count < 60; ++at) {
        uint16_t half = 0;
        std::memcpy(&half, slot + at, 2);
        if (ReadU32(slot + at) == value || (value < 0x10000 && half == value && ReadU32(slot + at) != value)) {
            found += " " + std::to_string(at) + (ReadU32(slot + at) == value ? "" : "h");
            ++count;
        }
    }
    mod::Log("[test] slot %d value %u at:%s", index, value, found.c_str());
}

namespace {

// What Copies needs from a record, read in one go while the memory is known to be there.
struct Found {
    const uint8_t* Address;
    uint64_t Played;
    char Player1[NameBytes + 1], Player2[NameBytes + 1];
};

// The game can free memory while it's being looked through, so a region is read under a guard and only plain bytes come out.
int ScanRegion(const uint8_t* begin, const uint8_t* end, Found* found, int room)
{
    int count = 0;
    __try {
        for (const uint8_t* at = begin; at + 0x100 <= end && count < room; at += 8) {
            if (ReadU32(at) != SlotTag || !LooksLikeSlot(at, end))
                continue;
            Found& f = found[count++];
            f.Address = at;
            f.Played = ReadU64(at + HeaderPlayed);
            f.Player1[0] = f.Player2[0] = 0;
            if (end - at > Player2Name + NameBytes) {
                memcpy(f.Player1, at + Player1Name, NameBytes);
                memcpy(f.Player2, at + Player2Name, NameBytes);
                f.Player1[NameBytes] = f.Player2[NameBytes] = 0;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return count;
}

} // namespace

std::vector<Copy> Copies()
{
    std::vector<Copy> copies;
    std::vector<Slot> slots;
    {
        std::lock_guard lock(g_mutex);
        if (!FindBlock())
            return copies;
        for (int i = 0; i < SlotCount(); ++i)
            if (ReadU32(SlotAt(i) + HeaderTag) == SlotTag)
                slots.push_back(Read(i, SlotAt(i)));
    }
    constexpr int Room = 64;
    std::vector<Found> found(Room);
    MEMORY_BASIC_INFORMATION info {};
    for (const uint8_t* address = nullptr; VirtualQuery(address, &info, sizeof(info)) == sizeof(info) && copies.size() < Room;
         address = static_cast<const uint8_t*>(info.BaseAddress) + info.RegionSize) {
        if (info.State != MEM_COMMIT || info.Protect != PAGE_READWRITE || info.RegionSize < 0x100 || info.RegionSize > (4ull << 30))
            continue;
        const auto* begin = static_cast<const uint8_t*>(info.BaseAddress);
        if (begin == g_block)
            continue;
        const int count = ScanRegion(begin, begin + info.RegionSize, found.data(), Room - static_cast<int>(copies.size()));
        for (int i = 0; i < count; ++i) {
            Copy copy;
            copy.Address = found[i].Address;
            copy.RegionSize = info.RegionSize;
            copy.RegionOffset = static_cast<size_t>(found[i].Address - begin);
            copy.Played = found[i].Played;
            copy.Player1 = NameAt(reinterpret_cast<const uint8_t*>(found[i].Player1));
            copy.Player2 = NameAt(reinterpret_cast<const uint8_t*>(found[i].Player2));
            for (const Slot& slot : slots)
                if (slot.Played == copy.Played && slot.Player1 == copy.Player1 && slot.Player2 == copy.Player2)
                    copy.Slot = slot.Index;
            copies.push_back(copy);
        }
    }
    return copies;
}

bool& OlderReplaysOn()
{
    static bool on = true;
    return on;
}

void StartOlderReplays()
{
    static std::atomic<bool> started { false };
    if (started.exchange(true))
        return;
    CloseHandle(CreateThread(nullptr, 0, [](void*) -> DWORD {
        for (;;) {
            Sleep(3000);
            if (OlderReplaysOn())
                UnlockOlderReplays();
            AutoLoadTick();
            AutoExportTick();
        }
    }, nullptr, 0, nullptr));
}

namespace {
std::atomic<int> g_autoLoaded { 0 };
} // namespace

bool& AutoLoadOn()
{
    static bool on = true;
    return on;
}

int AutoLoaded()
{
    return g_autoLoaded;
}

void AutoLoadTick()
{
    // Each file once per session (again if the file changes), so a file that can't load is logged once, not every few seconds.
    static std::map<std::string, std::filesystem::file_time_type> handled;
    static int lastCount = -1, steady = 0;
    if (!AutoLoadOn() || !Available())
        return;
    // The game fills its slots from the saves as it starts. Wait until the count has held for a few checks, so nothing is put in a
    // slot the game is about to fill.
    const int used = static_cast<int>(Slots().size());
    if (used != lastCount) {
        lastCount = used;
        steady = 0;
        return;
    }
    if (++steady < 3)
        return;

    for (const std::string& name : SharedFiles()) {
        std::error_code ignored;
        const auto written = std::filesystem::last_write_time(PathOf(name), ignored);
        const auto seen = handled.find(name);
        if (seen != handled.end() && seen->second == written)
            continue;
        handled[name] = written;
        std::string error;
        bool alreadyThere = false;
        if (Import(name, error, &alreadyThere)) {
            ++g_autoLoaded;
            mod::Log("Sharing: loaded %s on its own", name.c_str());
        } else if (!alreadyThere) {
            mod::Log("Sharing: couldn't load %s: %s", name.c_str(), error.c_str());
        }
    }
    lastCount = static_cast<int>(Slots().size());
}

bool Import(const std::string& fileName, std::string& error, bool* alreadyThere)
{
    if (alreadyThere)
        *alreadyThere = false;
    if (!mod::SafeFileName(fileName)) {
        error = "That isn't a tekken8mvm replay file";
        return false;
    }
    std::ifstream file(PathOf(fileName), std::ios::binary);
    FileHeader header;
    file.read(reinterpret_cast<char*>(&header), sizeof(header));
    if (!file || !HeaderOk(header)) {
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
    std::vector<uint8_t> data(g_slotSize);
    file.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(header.Stored));
    if (!file) {
        error = "The file is cut short";
        return false;
    }
    // The slot inside must look like the game's own, or the game's list of replays could be thrown off by it.
    if (ReadU32(data.data() + HeaderTag) != SlotTag || ReadU32(data.data() + HeaderSize) != g_slotSize || ReadU32(data.data() + HeaderGameVersion) != header.GameVersion) {
        error = "That isn't a tekken8mvm replay file";
        return false;
    }

    // Replays only play on the game version they were made on, so an older one is marked with this version (see UnlockOlderReplays).
    const uint32_t newest = NewestVersion();
    const Slot incoming = Read(-1, data.data());
    int last = -1;
    for (int i = 0; i < SlotCount(); ++i) {
        if (ReadU32(SlotAt(i) + HeaderTag) != SlotTag)
            continue;
        last = i;
        // The same fight already in the list, loaded before and kept by the game's own saving, or the player's own: not again.
        const Slot existing = Read(i, SlotAt(i));
        if (existing.Played == incoming.Played && existing.Player1 == incoming.Player1 && existing.Player2 == incoming.Player2) {
            if (alreadyThere)
                *alreadyThere = true;
            error = "This replay is already in the game's list";
            return false;
        }
    }
    // Right after the last used slot. Further along the block isn't all slots: the end of it holds other things.
    const int empty = last + 1;
    if (newest != 0 && header.GameVersion < newest && !OlderReplaysOn()) {
        error = "This replay is from an older version of the game (" + std::to_string(header.GameVersion) + ", yours is " + std::to_string(newest) + "). Tick \"Play replays from older versions\" to load it";
        return false;
    }
    if (empty < 0 || empty >= SlotCount() - 8) {
        error = "All the game's replay slots are full";
        return false;
    }
    // It shows up as a downloaded replay, dated now in the "when it came in" field.
    if (ReadU64(data.data() + HeaderDownloaded) == 0) {
        const uint64_t now = static_cast<uint64_t>(std::time(nullptr));
        std::memcpy(data.data() + HeaderDownloaded, &now, sizeof(now));
    }
    if (newest != 0 && header.GameVersion < newest) {
        g_realVersions.emplace(FightOf(data.data()), std::make_pair(header.GameVersion, ReadU32(data.data() + HeaderPlayVersion)));
        std::memcpy(data.data() + HeaderGameVersion, &newest, sizeof(newest));
        const uint32_t second = ReadU32(data.data() + HeaderPlayVersion);
        if (second >= 10000 && second < 100000)
            std::memcpy(data.data() + HeaderPlayVersion, &newest, sizeof(newest));
    }
    std::memcpy(SlotAt(empty), data.data(), g_slotSize);
    mod::Log("Sharing: imported %s into slot %d", fileName.c_str(), empty);
    return true;
}

namespace {
std::atomic<int> g_autoExported { 0 };
} // namespace

bool& AutoExportOn()
{
    static bool on = true;
    return on;
}

int AutoExported()
{
    return g_autoExported;
}

std::string MyFolder()
{
    return Utf8Of(MyFolderPath());
}

void AutoExportTick()
{
    // Every replay of your own in the game's list, once. The ones already in the folder are known by their fight (date and both
    // names), so nothing is written twice, across sessions too. A match that ends while the game runs is written within seconds of
    // the game putting it in its list; one played online, without the mod, is written the next time a session starts and the game
    // has loaded its replays.
    static std::set<Fight> known;
    static bool scanned = false;
    static int lastCount = -1, steady = 0;
    if (!AutoExportOn() || !Available())
        return;
    const std::vector<Slot> slots = Slots();
    const int used = static_cast<int>(slots.size());
    if (used != lastCount) {
        lastCount = used;
        steady = 0;
        return;
    }
    if (++steady < 3)
        return;
    const std::filesystem::path folder = MyFolderPath();
    if (!scanned) {
        scanned = true;
        std::error_code ignored;
        for (const auto& entry : std::filesystem::directory_iterator(folder, ignored)) {
            Slot slot;
            std::string error;
            if (entry.path().extension() == ".t8replay" && DescribePath(entry.path(), slot, error))
                known.insert({ slot.Played, slot.Player1, slot.Player2 });
        }
    }
    for (const Slot& slot : slots) {
        // A downloaded replay, or one loaded from a shared file, is someone else's.
        if (slot.Downloaded || slot.Played == 0)
            continue;
        const Fight fight { slot.Played, slot.Player1, slot.Player2 };
        if (known.count(fight))
            continue;
        known.insert(fight); // tried once a session, written or not
        std::lock_guard lock(g_mutex);
        if (!FindBlock())
            return;
        const Slot now = Read(slot.Index, SlotAt(slot.Index));
        if (now.Played != slot.Played || now.Player1 != slot.Player1 || now.Player2 != slot.Player2) {
            known.erase(fight); // the list moved since it was read: the next look finds it
            continue;
        }
        std::string error;
        if (WriteSlot(slot.Index, folder / Utf8Path(NameIn(folder, slot) + ".t8replay"), error))
            ++g_autoExported;
        else
            mod::Log("Sharing: couldn't export slot %d on its own: %s", slot.Index, error.c_str());
    }
}

} // namespace sharing
