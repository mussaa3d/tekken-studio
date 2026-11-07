#include "mod/pakmods.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>

#include "mod/log.h"
#include "mod/scan.h"

namespace pakmods {

namespace {

namespace fs = std::filesystem;

// The engine's string: characters, count with the terminating zero, capacity. Only read by the calls below.
struct FString {
    const wchar_t* Data;
    int32_t Num;
    int32_t Max;
};
using MountHandler = void*(__fastcall*)(void* self, const FString* path, int32_t order);
using UnmountHandler = bool(__fastcall*)(void* self, const FString* path);

struct Entry {
    Mod Info;
    fs::path File;       // full path on disk
    std::wstring Engine; // the name the engine mounted it under
    bool Want = true;
};

std::mutex g_mutex;
std::vector<Entry> g_mods;
bool g_listed = false;
std::string g_message;

std::atomic<int> g_state { 0 }; // 0 not looked, 1 looking, 2 found, 3 not found
MountHandler g_mount = nullptr;
UnmountHandler g_unmount = nullptr;
void* g_pakFile = nullptr;

fs::path PaksDir()
{
    wchar_t exe[MAX_PATH] = L"";
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    // <game>\Polaris\Binaries\Win64\Polaris-Win64-Shipping.exe -> <game>\Polaris\Content\Paks
    return fs::path(exe).parent_path().parent_path().parent_path() / L"Content" / L"Paks";
}

fs::path SettingsFile()
{
    return mod::DataDir() / "pak_mods_off.txt";
}

// Mods switched off stay off at the next start: the file lists them by name, one per line.
std::vector<std::string> SavedOff()
{
    std::vector<std::string> names;
    std::ifstream file(SettingsFile());
    for (std::string line; std::getline(file, line);)
        if (!line.empty())
            names.push_back(line);
    return names;
}

void SaveOff(const std::vector<Entry>& mods)
{
    std::ofstream file(SettingsFile(), std::ios::trunc);
    for (const Entry& entry : mods)
        if (!entry.Want)
            file << entry.Info.Name << '\n';
}

void ListFiles()
{
    std::vector<Entry> found;
    const fs::path paks = PaksDir();
    std::error_code error;
    // The game's own paks sit in Paks itself. Everything in a folder under it is a mod.
    for (fs::recursive_directory_iterator it(paks, fs::directory_options::skip_permission_denied, error), end; it != end; it.increment(error)) {
        if (error)
            break;
        if (!it->is_regular_file(error) || it->path().extension() != L".pak" || it->path().parent_path() == paks)
            continue;
        Entry entry;
        entry.File = it->path();
        entry.Info.Name = it->path().stem().string();
        entry.Info.Folder = fs::relative(it->path().parent_path(), paks, error).string();
        std::wstring relative = fs::relative(it->path(), paks, error).wstring();
        std::replace(relative.begin(), relative.end(), L'\\', L'/');
        entry.Engine = L"../../../Polaris/Content/Paks/" + relative;
        found.push_back(std::move(entry));
    }
    std::sort(found.begin(), found.end(), [](const Entry& a, const Entry& b) { return a.Info.Name < b.Info.Name; });
    const std::vector<std::string> off = SavedOff();
    for (Entry& entry : found)
        if (std::find(off.begin(), off.end(), entry.Info.Name) != off.end())
            entry.Want = false;
    std::lock_guard lock(g_mutex);
    // Keep what was switched off when listing again.
    for (Entry& entry : found)
        for (const Entry& old : g_mods)
            if (old.File == entry.File) {
                entry.Info.Mounted = old.Info.Mounted;
                entry.Want = old.Want;
            }
    g_mods = std::move(found);
    g_listed = true;
}

// The function that logs this text, found from the text itself so a patch that moves code doesn't matter.
uintptr_t FunctionLogging(const scan::Module& module, const wchar_t* text)
{
    const size_t bytes = wcslen(text) * sizeof(wchar_t);
    const auto* begin = reinterpret_cast<const uint8_t*>(module.Base);
    const uint8_t* string = std::search(begin, begin + module.Size, reinterpret_cast<const uint8_t*>(text), reinterpret_cast<const uint8_t*>(text) + bytes);
    if (string == begin + module.Size)
        return 0;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(begin);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(begin + dos->e_lfanew);
    const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
        if (!(section->Characteristics & IMAGE_SCN_MEM_EXECUTE))
            continue;
        const uint8_t* code = begin + section->VirtualAddress;
        const size_t size = section->Misc.VirtualSize;
        for (size_t at = 0; at + 7 <= size; ++at) {
            // lea r64, [rip + disp32]
            if ((code[at] != 0x48 && code[at] != 0x4C) || code[at + 1] != 0x8D || (code[at + 2] & 0xC7) != 0x05)
                continue;
            int32_t displacement;
            std::memcpy(&displacement, code + at + 3, 4);
            if (code + at + 7 + displacement != string)
                continue;
            DWORD64 imageBase = 0;
            const RUNTIME_FUNCTION* function = RtlLookupFunctionEntry(reinterpret_cast<DWORD64>(code + at), &imageBase, nullptr);
            return function ? static_cast<uintptr_t>(imageBase + function->BeginAddress) : 0;
        }
    }
    return 0;
}

// The pak manager registers itself with the engine's mount delegate, which keeps the object and this method side by side.
uintptr_t FindPointerTo(uintptr_t value, uintptr_t moduleBase, size_t moduleSize)
{
    MEMORY_BASIC_INFORMATION info {};
    for (uint8_t* at = nullptr; VirtualQuery(at, &info, sizeof(info)) == sizeof(info); at = static_cast<uint8_t*>(info.BaseAddress) + info.RegionSize) {
        if (info.State != MEM_COMMIT || info.Type != MEM_PRIVATE || info.Protect != PAGE_READWRITE || info.RegionSize > (1ull << 31))
            continue;
        const auto* words = static_cast<const uintptr_t*>(info.BaseAddress);
        const size_t count = info.RegionSize / sizeof(uintptr_t);
        __try {
            for (size_t i = 1; i < count; ++i) {
                if (words[i] != value)
                    continue;
                const uintptr_t object = words[i - 1];
                if (object < 0x10000 || (object & 7))
                    continue;
                if (!scan::IsReadable(reinterpret_cast<const void*>(object), 8))
                    continue;
                const uintptr_t vtable = *reinterpret_cast<const uintptr_t*>(object);
                if (vtable >= moduleBase && vtable < moduleBase + moduleSize)
                    return object;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    return 0;
}

void Find()
{
    const scan::Module module = scan::MainModule();
    const uintptr_t mount = FunctionLogging(module, L"Mounting pak file: %s");
    const uintptr_t unmount = FunctionLogging(module, L"Unmounting pak file: %s");
    std::string message;
    void* pakFile = nullptr;
    if (!mount || !unmount) {
        message = "The game's pak manager wasn't found in this game version.";
    } else {
        pakFile = reinterpret_cast<void*>(FindPointerTo(mount, module.Base, module.Size));
        if (!pakFile)
            message = "The game's pak manager wasn't found in memory.";
    }
    {
        std::lock_guard lock(g_mutex);
        g_message = message;
    }
    if (!pakFile) {
        mod::Log("Pak mods: %s", message.c_str());
        g_state = 3;
        return;
    }
    g_mount = reinterpret_cast<MountHandler>(mount);
    g_unmount = reinterpret_cast<UnmountHandler>(unmount);
    g_pakFile = pakFile;
    mod::Log("Pak mods: pak manager at %p", pakFile);
    g_state = 2;
}

void StartLooking()
{
    int expected = 0;
    if (g_state.compare_exchange_strong(expected, 1))
        std::thread(Find).detach();
}

bool CallMount(const std::wstring& path, bool mount, bool& result)
{
    const FString text { path.c_str(), static_cast<int32_t>(path.size() + 1), static_cast<int32_t>(path.size() + 1) };
    __try {
        result = mount ? g_mount(g_pakFile, &text, -1) != nullptr : g_unmount(g_pakFile, &text);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

} // namespace

std::vector<Mod> List()
{
    StartLooking();
    bool listed;
    {
        std::lock_guard lock(g_mutex);
        listed = g_listed;
    }
    if (!listed)
        ListFiles();
    std::lock_guard lock(g_mutex);
    std::vector<Mod> mods;
    for (const Entry& entry : g_mods) {
        Mod mod = entry.Info;
        mod.Busy = entry.Want != entry.Info.Mounted;
        mods.push_back(mod);
    }
    return mods;
}

void Rescan()
{
    ListFiles();
}

void SetMounted(const std::string& name, bool mounted)
{
    std::lock_guard lock(g_mutex);
    for (Entry& entry : g_mods)
        if (entry.Info.Name == name)
            entry.Want = mounted;
    SaveOff(g_mods);
}

bool Available()
{
    StartLooking();
    return g_state == 2;
}

std::string LastMessage()
{
    std::lock_guard lock(g_mutex);
    return g_message;
}

void OnGameTick()
{
    // Looks for the pak manager from the start, so mods switched off last time are unmounted before a fight loads them.
    static bool started = false;
    if (!started) {
        started = true;
        StartLooking();
    }
    if (g_state != 2)
        return;
    static bool listed = false;
    if (!listed) {
        listed = true;
        bool have;
        {
            std::lock_guard lock(g_mutex);
            have = g_listed;
        }
        if (!have)
            ListFiles();
    }
    std::lock_guard lock(g_mutex);
    for (Entry& entry : g_mods) {
        if (entry.Want == entry.Info.Mounted)
            continue;
        const bool wanted = entry.Want;
        // The engine knows each pak by the path it found it under, which is relative to the game's binaries.
        std::wstring absolute = entry.File.wstring();
        std::replace(absolute.begin(), absolute.end(), L'\\', L'/');
        bool done = false, crashed = false;
        for (const std::wstring& path : { entry.Engine, absolute }) {
            bool result = false;
            if (!CallMount(path, entry.Want, result)) {
                crashed = true;
                break;
            }
            if (result) {
                done = true;
                break;
            }
        }
        if (done) {
            entry.Info.Mounted = entry.Want;
            g_message = entry.Info.Name + (entry.Want ? " is on again." : " is off.") + " Things already loaded change when the game loads them again.";
        } else {
            entry.Want = entry.Info.Mounted;
            g_message = crashed ? "The game refused that change." : "The game didn't recognise " + entry.Info.Name + ".";
        }
        mod::Log("Pak mods: %s %s: %s", wanted ? "mount" : "unmount", entry.Info.Name.c_str(), done ? "done" : (crashed ? "fault" : "not found"));
    }
}

} // namespace pakmods
