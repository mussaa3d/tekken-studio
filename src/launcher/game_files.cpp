#include "launcher/game_files.h"

#include <windows.h>

#include <shlobj.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <map>
#include <sstream>

namespace files {

namespace {

// Tokens of a VDF file: every quoted string in order, with \\ unescaped.
std::vector<std::string> QuotedStrings(const std::string& text)
{
    std::vector<std::string> tokens;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '"')
            continue;
        std::string token;
        for (++i; i < text.size() && text[i] != '"'; ++i) {
            if (text[i] == '\\' && i + 1 < text.size())
                ++i;
            token.push_back(text[i]);
        }
        tokens.push_back(std::move(token));
    }
    return tokens;
}

std::string Lower(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

constexpr const char* Marker = "TEKKEN8MVM-BUILD-MARKER";
constexpr const char* OldMarker = "T8CINE-BUILD-MARKER"; // the name this tool had before
constexpr const wchar_t* BackupName = L"dinput8.dll.tekken8mvm-backup";

uint64_t FolderBytes(const fs::path& folder, int* fileCount = nullptr)
{
    uint64_t total = 0;
    std::error_code error;
    for (const auto& entry : fs::recursive_directory_iterator(folder, fs::directory_options::skip_permission_denied, error)) {
        if (entry.is_regular_file(error)) {
            total += entry.file_size(error);
            if (fileCount)
                ++*fileCount;
        }
    }
    return total;
}

std::string NowText()
{
    char text[32];
    const std::time_t now = std::time(nullptr);
    std::tm local {};
    localtime_s(&local, &now);
    std::strftime(text, sizeof(text), "%Y-%m-%d %H:%M", &local);
    return text;
}

InstallResult Fail(const std::string& message)
{
    return { false, message };
}

} // namespace

std::vector<fs::path> ParseLibraryFolders(const std::string& text)
{
    std::vector<fs::path> paths;
    const auto tokens = QuotedStrings(text);
    for (size_t i = 0; i + 1 < tokens.size(); ++i)
        if (Lower(tokens[i]) == "path")
            paths.emplace_back(tokens[i + 1]);
    return paths;
}

std::string VdfValue(const std::string& text, const std::string& key)
{
    const auto tokens = QuotedStrings(text);
    const std::string wanted = Lower(key);
    for (size_t i = 0; i + 1 < tokens.size(); ++i)
        if (Lower(tokens[i]) == wanted)
            return tokens[i + 1];
    return {};
}

bool LoginUsersOffline(const std::string& text, const std::string& autoLoginUser)
{
    // Each account is a block: "7656..." { "AccountName" "x" "AutoLogin" "1" "WantsOfflineMode" "1" ... }
    std::vector<std::map<std::string, std::string>> accounts;
    std::istringstream stream(text);
    std::string line;
    std::map<std::string, std::string> block;
    while (std::getline(stream, line)) {
        const auto tokens = QuotedStrings(line);
        if (tokens.size() == 2) {
            block[Lower(tokens[0])] = tokens[1];
        } else if (line.find('}') != std::string::npos && !block.empty()) {
            accounts.push_back(block);
            block.clear();
        }
    }

    const auto find = [&](const char* key, const std::string& value) -> const std::map<std::string, std::string>* {
        for (const auto& account : accounts) {
            const auto it = account.find(key);
            if (it != account.end() && Lower(it->second) == Lower(value))
                return &account;
        }
        return nullptr;
    };
    const std::map<std::string, std::string>* current = autoLoginUser.empty() ? nullptr : find("accountname", autoLoginUser);
    if (!current)
        current = find("mostrecent", "1");
    if (!current)
        current = find("autologin", "1");
    if (!current) {
        unsigned long long newest = 0;
        for (const auto& account : accounts) {
            const auto it = account.find("timestamp");
            const unsigned long long stamp = it == account.end() ? 0 : std::strtoull(it->second.c_str(), nullptr, 10);
            if (stamp >= newest) {
                newest = stamp;
                current = &account;
            }
        }
    }
    if (!current)
        return false;
    const auto offline = current->find("wantsofflinemode");
    return offline != current->end() && offline->second == "1";
}

std::string ReadTextFile(const fs::path& path)
{
    std::ifstream file(path, std::ios::binary);
    std::stringstream contents;
    contents << file.rdbuf();
    return contents.str();
}

DownloadEstimate EstimateDownload(const std::string& log)
{
    DownloadEstimate estimate;
    const std::string appLine = std::string("AppID 1778820 update started : download ");
    const size_t started = log.rfind(appLine);
    if (started == std::string::npos)
        return estimate;
    const char* numbers = log.c_str() + started + appLine.size();
    char* end = nullptr;
    estimate.Done = std::strtoull(numbers, &end, 10);
    if (*end != '/')
        return estimate;
    estimate.Total = std::strtoull(end + 1, nullptr, 10);
    estimate.Known = estimate.Total > 0;

    constexpr const char* RateLine = "Current download rate: ";
    size_t at = started;
    while ((at = log.find(RateLine, at)) != std::string::npos) {
        at += std::char_traits<char>::length(RateLine);
        estimate.Mbps = std::strtod(log.c_str() + at, nullptr);
        estimate.Done += static_cast<uint64_t>(estimate.Mbps * 1000000.0 / 8.0 * 60.0);
    }
    estimate.Done = std::min(estimate.Done, estimate.Total);
    return estimate;
}

SteamState FindSteam()
{
    SteamState steam;
    wchar_t path[MAX_PATH] = L"";
    DWORD size = sizeof(path);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", RRF_RT_REG_SZ, nullptr, path, &size) == ERROR_SUCCESS)
        steam.SteamDir = path;
    if (steam.SteamDir.empty() || !fs::exists(steam.SteamDir))
        steam.SteamDir = L"C:\\Program Files (x86)\\Steam";
    wchar_t autoLogin[256] = L"";
    size = sizeof(autoLogin);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"AutoLoginUser", RRF_RT_REG_SZ, nullptr, autoLogin, &size);
    steam.SteamOffline = LoginUsersOffline(ReadTextFile(steam.SteamDir / "config" / "loginusers.vdf"), fs::path(autoLogin).string());
    return steam;
}

GameInstall FindGame(const SteamState& steam)
{
    GameInstall game;
    std::vector<fs::path> libraries = ParseLibraryFolders(ReadTextFile(steam.SteamDir / "steamapps" / "libraryfolders.vdf"));
    libraries.push_back(steam.SteamDir);
    for (const fs::path& library : libraries) {
        const fs::path manifest = library / "steamapps" / (std::wstring(L"appmanifest_") + AppId + L".acf");
        if (!fs::exists(manifest))
            continue;
        const std::string text = ReadTextFile(manifest);
        game.Found = true;
        game.Root = library / "steamapps" / "common" / VdfValue(text, "installdir");
        game.Win64 = game.Root / "Polaris" / "Binaries" / "Win64";
        game.Paks = game.Root / "Polaris" / "Content" / "Paks";
        game.BuildId = VdfValue(text, "buildid");
        game.StateFlags = std::atoi(VdfValue(text, "StateFlags").c_str());
        game.BytesDownloaded = std::strtoull(VdfValue(text, "BytesDownloaded").c_str(), nullptr, 10);
        game.BytesToDownload = std::strtoull(VdfValue(text, "BytesToDownload").c_str(), nullptr, 10);
        break;
    }
    return game;
}

fs::path DataDir(const GameInstall& game)
{
    return game.Win64 / "tekken8mvm";
}

fs::path SkinMountDir(const GameInstall& game)
{
    return game.Paks / "~tekken8mvm";
}

namespace {

// Reads a file the way a loaded DLL allows: the game, or a virus scanner looking at a new file, keeps it open, and a plain read
// can be refused then. False if it can't be opened at all.
bool ReadShared(const fs::path& path, std::string& contents)
{
    contents.clear();
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    LARGE_INTEGER size {};
    bool ok = GetFileSizeEx(file, &size) && size.QuadPart < (1ll << 30);
    if (ok) {
        contents.resize(static_cast<size_t>(size.QuadPart));
        DWORD read = 0;
        ok = contents.empty() || (ReadFile(file, contents.data(), static_cast<DWORD>(contents.size()), &read, nullptr) && read == contents.size());
    }
    CloseHandle(file);
    return ok;
}

bool HasMarker(const std::string& contents)
{
    return contents.find(Marker) != std::string::npos || contents.find(OldMarker) != std::string::npos;
}

// A DLL the game has just let go of can stay busy for a moment, so each step gets a few tries over about five seconds.
template <typename Step> bool WithRetries(Step step)
{
    for (int attempt = 0; attempt < 10; ++attempt) {
        if (step())
            return true;
        Sleep(500);
    }
    return false;
}

} // namespace

bool IsOurDll(const fs::path& path)
{
    std::string contents;
    return ReadShared(path, contents) && HasMarker(contents);
}

bool ModFilesPresent(const GameInstall& game)
{
    std::error_code error;
    const fs::path dll = game.Win64 / "dinput8.dll";
    return (fs::exists(dll, error) && IsOurDll(dll)) || fs::exists(SkinMountDir(game), error);
}

std::vector<std::string> OtherMods(const GameInstall& game)
{
    std::vector<std::string> found;
    std::error_code error;
    // The game's own paks sit directly in Paks, so anything in a folder under it is a mod.
    for (const auto& entry : fs::directory_iterator(game.Paks, error)) {
        if (!entry.is_directory(error) || entry.path().filename() == "~tekken8mvm")
            continue;
        for (const auto& file : fs::recursive_directory_iterator(entry.path(), fs::directory_options::skip_permission_denied, error))
            if (Lower(file.path().extension().string()) == ".pak")
                found.push_back(file.path().stem().string());
    }
    const fs::path dll = game.Win64 / "dinput8.dll";
    if (fs::exists(dll, error) && !IsOurDll(dll))
        found.push_back("dinput8.dll (another mod's loader)");
    std::sort(found.begin(), found.end());
    return found;
}

void SwitchGameSettings(const fs::path& profilesDir, const fs::path& liveFile, const std::string& mode)
{
    std::error_code error;
    fs::create_directories(profilesDir, error);
    const fs::path activeFile = profilesDir / "active.txt";
    std::string active = ReadTextFile(activeFile);
    active.erase(std::remove_if(active.begin(), active.end(), [](unsigned char c) { return std::isspace(c) != 0; }), active.end());
    if (active.empty())
        active = "normal";
    if (active == mode)
        return;
    const std::string name = liveFile.filename().string();
    if (fs::exists(liveFile, error)) {
        fs::create_directories(profilesDir / active, error);
        fs::copy_file(liveFile, profilesDir / active / name, fs::copy_options::overwrite_existing, error);
        if (error)
            return; // nothing's been changed, and the file is still safe where it is
    }
    if (fs::exists(profilesDir / mode / name, error)) {
        fs::copy_file(profilesDir / mode / name, liveFile, fs::copy_options::overwrite_existing, error);
        if (error)
            return;
    }
    std::ofstream(activeFile, std::ios::trunc) << mode << "\n";
}

fs::path GameSettingsFile()
{
    return SaveGamesDir().parent_path() / "Config" / "Windows" / "GameUserSettings.ini";
}

void MoveFromOldName(const GameInstall& game)
{
    std::error_code error;
    if (fs::exists(game.Win64 / "T8Cine", error) && !fs::exists(DataDir(game), error)) {
        fs::rename(game.Win64 / "T8Cine", DataDir(game), error);
        if (!error) {
            std::error_code ignored;
            fs::rename(DataDir(game) / "t8cine.ini", DataDir(game) / "tekken8mvm.ini", ignored);
        }
    }
    if (fs::exists(game.Paks / "~T8Cine", error))
        fs::remove_all(game.Paks / "~T8Cine", error);
    if (fs::exists(game.Win64 / L"dinput8.dll.t8cine-backup", error) && !fs::exists(game.Win64 / BackupName, error))
        fs::rename(game.Win64 / L"dinput8.dll.t8cine-backup", game.Win64 / BackupName, error);
}

InstallResult InstallMod(const GameInstall& game, const fs::path& modDll, const std::vector<fs::path>& skinFolders)
{
    std::error_code error;
    if (!fs::exists(modDll, error))
        return Fail("The mod DLL is missing: " + modDll.string());
    if (!fs::exists(game.Win64, error))
        return Fail("The game folder isn't there yet: " + game.Win64.string());

    const fs::path target = game.Win64 / "dinput8.dll";
    if (fs::exists(target, error)) {
        // Only a dinput8.dll that's surely another mod's gets set aside. One that can't be read yet is waited for, not guessed at.
        std::string contents;
        if (!WithRetries([&] { return ReadShared(target, contents); }))
            return Fail("The game folder's dinput8.dll is in use, so the game is probably still closing. Wait a few seconds and try again.");
        if (!HasMarker(contents) && !WithRetries([&] {
                error.clear();
                fs::rename(target, game.Win64 / BackupName, error);
                return !error;
            }))
            return Fail("Couldn't set aside the other dinput8.dll (" + error.message() + "). If the game is still closing, wait a few seconds and try again.");
    }
    if (!WithRetries([&] {
            error.clear();
            fs::copy_file(modDll, target, fs::copy_options::overwrite_existing, error);
            return !error;
        }))
        return Fail("Couldn't copy the mod in (is the game still running?): " + error.message());

    fs::create_directories(DataDir(game), error);
    for (const fs::path& folder : skinFolders) {
        const fs::path destination = SkinMountDir(game) / folder.filename();
        fs::create_directories(destination, error);
        fs::copy(folder, destination, fs::copy_options::overwrite_existing | fs::copy_options::recursive, error);
        if (error)
            return Fail("Couldn't copy skin " + folder.filename().string() + ": " + error.message());
    }

    // Last, so the mod can only wake up when everything above worked.
    std::ofstream(DataDir(game) / "session.lock", std::ios::trunc) << NowText() << "\n";
    return { true, "Mod installed" };
}

InstallResult RemoveMod(const GameInstall& game)
{
    std::error_code error;
    if (!game.Found)
        return { true, "" };
    fs::remove(DataDir(game) / "session.lock", error);

    const fs::path target = game.Win64 / "dinput8.dll";
    if (fs::exists(target, error) && IsOurDll(target) && !WithRetries([&] {
            error.clear();
            fs::remove(target, error);
            return !error;
        }))
        return Fail("Couldn't remove the mod (is the game still running?): " + error.message());
    if (fs::exists(game.Win64 / BackupName, error) && !fs::exists(target, error)) {
        fs::rename(game.Win64 / BackupName, target, error);
        if (error)
            return Fail("Couldn't restore the other dinput8.dll: " + error.message());
    }
    if (fs::exists(SkinMountDir(game), error)) {
        fs::remove_all(SkinMountDir(game), error);
        if (error)
            return Fail("Couldn't remove the skins: " + error.message());
    }
    return { true, "Mod removed" };
}

std::vector<Skin> ListSkins(const fs::path& skinsDir)
{
    std::vector<Skin> skins;
    std::error_code error;
    for (const auto& entry : fs::directory_iterator(skinsDir, error)) {
        if (!entry.is_directory(error))
            continue;
        Skin skin;
        skin.Name = entry.path().filename().string();
        skin.Folder = entry.path();
        skin.Bytes = FolderBytes(entry.path());
        for (const auto& file : fs::recursive_directory_iterator(entry.path(), error))
            if (Lower(file.path().extension().string()) == ".pak")
                ++skin.PakCount;
        skins.push_back(std::move(skin));
    }
    std::sort(skins.begin(), skins.end(), [](const Skin& a, const Skin& b) { return a.Name < b.Name; });
    return skins;
}

fs::path SaveGamesDir()
{
    wchar_t* local = nullptr;
    fs::path result;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local)))
        result = fs::path(local) / "TEKKEN 8" / "Saved" / "SaveGames";
    CoTaskMemFree(local);
    return result;
}

std::vector<SavedReplay> ListSavedReplays(const fs::path& saveDir)
{
    std::vector<std::pair<fs::file_time_type, SavedReplay>> found;
    std::error_code error;
    for (const auto& entry : fs::recursive_directory_iterator(saveDir, fs::directory_options::skip_permission_denied, error)) {
        const std::string name = entry.path().filename().string();
        if (!entry.is_regular_file(error) || name.rfind("replay_game_no", 0) != 0 || entry.path().extension() != ".sav")
            continue;
        const auto modified = entry.last_write_time(error);
        const auto system = std::chrono::clock_cast<std::chrono::system_clock>(modified);
        const std::time_t time = std::chrono::system_clock::to_time_t(system);
        std::tm local {};
        localtime_s(&local, &time);
        char text[32];
        std::strftime(text, sizeof(text), "%Y-%m-%d %H:%M", &local);
        found.push_back({ modified, { name, text, entry.file_size(error) } });
    }
    std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    std::vector<SavedReplay> replays;
    for (auto& item : found)
        replays.push_back(std::move(item.second));
    return replays;
}

std::vector<VaultEntry> ListVault(const fs::path& vaultDir)
{
    std::vector<VaultEntry> entries;
    std::error_code error;
    for (const auto& entry : fs::directory_iterator(vaultDir, error)) {
        if (!entry.is_directory(error))
            continue;
        const std::string info = ReadTextFile(entry.path() / "vault.txt");
        VaultEntry item;
        item.Name = entry.path().filename().string();
        item.Folder = entry.path();
        std::istringstream lines(info);
        std::getline(lines, item.BuildId);
        std::getline(lines, item.Created);
        std::erase(item.BuildId, '\r');
        std::erase(item.Created, '\r');
        item.Bytes = FolderBytes(entry.path() / "saves", &item.Files);
        entries.push_back(std::move(item));
    }
    std::sort(entries.begin(), entries.end(), [](const VaultEntry& a, const VaultEntry& b) { return a.Created > b.Created; });
    return entries;
}

InstallResult SnapshotSaves(const fs::path& saveDir, const fs::path& vaultDir, const std::string& name, const std::string& buildId)
{
    std::error_code error;
    if (!fs::exists(saveDir, error))
        return Fail("No save folder yet: " + saveDir.string());
    if (name.empty() || name[0] == '.' || name.find_first_of("\\/:*?\"<>|") != std::string::npos)
        return Fail("Use a snapshot name without \\ / : * ? \" < > |");
    const fs::path folder = vaultDir / name;
    if (fs::exists(folder, error))
        return Fail("A snapshot called " + name + " already exists");
    fs::create_directories(folder / "saves", error);
    fs::copy(saveDir, folder / "saves", fs::copy_options::recursive, error);
    if (error)
        return Fail("Couldn't copy the saves: " + error.message());
    std::ofstream(folder / "vault.txt", std::ios::trunc) << buildId << "\n" << NowText() << "\n";
    return { true, "Saved snapshot " + name };
}

InstallResult RestoreSnapshot(const fs::path& saveDir, const fs::path& vaultDir, const VaultEntry& entry, const std::string& currentBuildId)
{
    std::error_code error;
    char stamp[32];
    const std::time_t now = std::time(nullptr);
    std::tm local {};
    localtime_s(&local, &now);
    std::strftime(stamp, sizeof(stamp), "auto-backup-%Y%m%d-%H%M%S", &local);
    if (fs::exists(saveDir, error)) {
        const InstallResult backup = SnapshotSaves(saveDir, vaultDir, stamp, currentBuildId);
        if (!backup.Ok)
            return backup;
    }

    fs::remove_all(saveDir, error);
    if (error)
        return Fail("Couldn't clear the live saves (is the game running?): " + error.message());
    fs::create_directories(saveDir, error);
    fs::copy(entry.Folder / "saves", saveDir, fs::copy_options::recursive, error);
    if (error)
        return Fail("Couldn't restore the saves: " + error.message());

    std::string message = "Restored " + entry.Name + ". Your previous saves are in " + stamp + ".";
    if (!entry.BuildId.empty() && entry.BuildId != currentBuildId)
        message += " This snapshot is from an older game build, so its replays may not play.";
    return { true, message };
}

} // namespace files
