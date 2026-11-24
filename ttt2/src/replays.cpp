// Replays kept and passed round, as in the Tekken 8 studio. BlueSpark writes a .ttt2replay for every offline Versus match and every
// online match into its own folder inside the play test, which is replaced when the play test is. The studio keeps a copy of each
// in the user's Videos folder (Videos\tekken8mvm\ttt2_replays), where they outlast the play test and can be sent to someone, and
// brings a file from anywhere into BlueSpark's folder so it can be watched. Nothing of BlueSpark's is changed: files are only
// copied, and a file in its folder is never written over.
#include "studio.h"

#include "bluespark.h"

#include <windows.h>
#include <commdlg.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <thread>

#pragma comment(lib, "comdlg32.lib")

namespace studio {

namespace {

std::atomic<bool> g_keep { true };
std::atomic<int> g_kept { 0 }, g_broughtIn { 0 };
std::atomic<bool> g_picking { false };

bool IsReplay(const std::filesystem::path& file)
{
    return file.extension() == ".ttt2replay";
}

// Whether two files are the same replay: BlueSpark names them by the time they were played, so the name and the size settle it.
bool Same(const std::filesystem::path& a, const std::filesystem::path& b)
{
    std::error_code ignored;
    return std::filesystem::is_regular_file(a, ignored) && std::filesystem::is_regular_file(b, ignored)
           && std::filesystem::file_size(a, ignored) == std::filesystem::file_size(b, ignored);
}

// A place for the file in the folder: its own name, or the name with a number where another replay already has it. Empty if the
// same replay is there already.
std::filesystem::path PlaceIn(const std::filesystem::path& folder, const std::filesystem::path& file)
{
    std::error_code ignored;
    std::filesystem::path target = folder / file.filename();
    for (int n = 2; std::filesystem::exists(target, ignored); ++n) {
        if (Same(target, file))
            return {};
        target = folder / (file.stem().wstring() + L" (" + std::to_wstring(n) + L")" + file.extension().wstring());
    }
    return target;
}

bool CopyTo(const std::filesystem::path& folder, const std::filesystem::path& file)
{
    std::error_code failed;
    std::filesystem::create_directories(folder, failed);
    const std::filesystem::path target = PlaceIn(folder, file);
    if (target.empty())
        return false;
    // Under another name first, so a half-written copy is never taken for a replay.
    const std::filesystem::path part = target.wstring() + L".part";
    std::filesystem::copy_file(file, part, std::filesystem::copy_options::overwrite_existing, failed);
    if (failed)
        return false;
    std::filesystem::rename(part, target, failed);
    if (failed) {
        std::filesystem::remove(part, failed);
        return false;
    }
    return true;
}

void Keeper()
{
    if (FILE* file = _wfopen((DataDir() / "keep-replays.txt").c_str(), L"r")) {
        g_keep = fgetc(file) != '0';
        fclose(file);
    }
    for (;;) {
        if (g_keep) {
            std::error_code ignored;
            const auto now = std::filesystem::file_time_type::clock::now();
            // The folder can go while it's being read (a play test being replaced): that look is given up, the next one starts over.
            try {
                for (const auto& entry : std::filesystem::recursive_directory_iterator(Ttt2ReplaysFolder(), ignored)) {
                    // One that's still being written (the match has only just ended) waits for the next look.
                    if (!entry.is_regular_file(ignored) || !IsReplay(entry.path()) || now - entry.last_write_time(ignored) < std::chrono::seconds(4))
                        continue;
                    if (CopyTo(SavedReplaysFolder(), entry.path())) {
                        ++g_kept;
                        Log("Replays: kept a copy of %s", Utf8(entry.path().filename()).c_str());
                    }
                }
            } catch (const std::exception&) {
            }
        }
        Sleep(3000);
    }
}

} // namespace

const std::filesystem::path& SavedReplaysFolder()
{
    static const std::filesystem::path folder = [] {
        wchar_t profile[MAX_PATH] = L".";
        GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH);
        return std::filesystem::path(profile) / "Videos" / "tekken8mvm" / "ttt2_replays";
    }();
    return folder;
}

bool KeepsReplays()
{
    return g_keep;
}

void KeepReplays(bool on)
{
    g_keep = on;
    if (FILE* file = _wfopen((DataDir() / "keep-replays.txt").c_str(), L"w")) {
        fputc(on ? '1' : '0', file);
        fclose(file);
    }
}

int ReplaysKept()
{
    return g_kept;
}

int ReplaysBroughtIn()
{
    return g_broughtIn;
}

bool SaveReplayCopy(const std::filesystem::path& file)
{
    return CopyTo(SavedReplaysFolder(), file);
}

bool ReplayIsSaved(const std::filesystem::path& file)
{
    return Same(SavedReplaysFolder() / file.filename(), file);
}

// Where the replay is in BlueSpark's folder, copied there first if it isn't. Empty if it couldn't be.
std::filesystem::path BringReplayIn(const std::filesystem::path& file)
{
    std::error_code ignored;
    if (!IsReplay(file) || !std::filesystem::is_regular_file(file, ignored))
        return {};
    const std::filesystem::path folder = Ttt2ReplaysFolder();
    // Already there, under its own name or anywhere in the folder's own sorting.
    for (const auto& entry : std::filesystem::recursive_directory_iterator(folder, ignored))
        if (entry.is_regular_file(ignored) && entry.path().filename() == file.filename() && Same(entry.path(), file))
            return entry.path();
    if (!CopyTo(folder, file))
        return {};
    ++g_broughtIn;
    for (const auto& entry : std::filesystem::directory_iterator(folder, ignored))
        if (entry.is_regular_file(ignored) && Same(entry.path(), file) && entry.path().stem().wstring().rfind(file.stem().wstring(), 0) == 0)
            return entry.path();
    return {};
}

// The saved copies that aren't in BlueSpark's folder (after a new play test, or files someone sent that were put in the folder).
std::vector<std::filesystem::path> SavedReplaysNotIn()
{
    std::vector<std::filesystem::path> files;
    std::error_code ignored;
    std::vector<std::filesystem::path> theirs;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(Ttt2ReplaysFolder(), ignored))
        if (entry.is_regular_file(ignored) && IsReplay(entry.path()))
            theirs.push_back(entry.path());
    for (const auto& entry : std::filesystem::directory_iterator(SavedReplaysFolder(), ignored)) {
        if (!entry.is_regular_file(ignored) || !IsReplay(entry.path()))
            continue;
        bool there = false;
        for (const auto& other : theirs)
            there = there || (other.filename() == entry.path().filename() && Same(other, entry.path()));
        if (!there)
            files.push_back(entry.path());
    }
    std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) { return a.filename() > b.filename(); });
    return files;
}

// Windows' own file window, on a thread of its own so the game goes on drawing; the files chosen are brought in.
void PickReplaysToBringIn()
{
    if (g_picking.exchange(true))
        return;
    std::thread([] {
        std::vector<wchar_t> buffer(32768, 0);
        OPENFILENAMEW ask {};
        ask.lStructSize = sizeof(ask);
        ask.lpstrFilter = L"TTT2 replays (*.ttt2replay)\0*.ttt2replay\0";
        ask.lpstrFile = buffer.data();
        ask.nMaxFile = static_cast<DWORD>(buffer.size());
        ask.lpstrTitle = L"Replays to add";
        ask.Flags = OFN_EXPLORER | OFN_ALLOWMULTISELECT | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
        if (GetOpenFileNameW(&ask)) {
            // One file: its whole path. Several: the folder, then each name.
            const std::filesystem::path first = buffer.data();
            const wchar_t* next = buffer.data() + wcslen(buffer.data()) + 1;
            if (!*next) {
                BringReplayIn(first);
            } else {
                for (; *next; next += wcslen(next) + 1)
                    BringReplayIn(first / next);
            }
        }
        g_picking = false;
    }).detach();
}

void StartReplayKeeper()
{
    std::thread(Keeper).detach();
}

} // namespace studio
