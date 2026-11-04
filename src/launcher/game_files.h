#pragma once
// Everything the launcher does to files: finding the game, reading Steam's state, putting the mod and skins
// in place for a session and taking them out again, and archiving replay saves.
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace files {

namespace fs = std::filesystem;

constexpr const wchar_t* AppId = L"1778820";
constexpr const wchar_t* GameExe = L"Polaris-Win64-Shipping.exe";

// Pulls the "path" entries out of Steam's libraryfolders.vdf text.
std::vector<fs::path> ParseLibraryFolders(const std::string& vdfText);

// Reads a top-level "key" "value" from an appmanifest or similar VDF text. Empty if missing.
std::string VdfValue(const std::string& vdfText, const std::string& key);

// True when the account Steam signs in with is set to Offline Mode. autoLoginUser is the AutoLoginUser value from the
// registry. Failing that: the block marked MostRecent or AutoLogin (Steam versions differ), then the newest timestamp.
bool LoginUsersOffline(const std::string& vdfText, const std::string& autoLoginUser = {});

std::string ReadTextFile(const fs::path& path);

// Steam doesn't keep the manifest's byte counts current during a download, so this adds up the per-minute
// rates in its content log since the update started. An estimate, for showing rough progress.
struct DownloadEstimate {
    uint64_t Done = 0, Total = 0;
    double Mbps = 0;
    bool Known = false;
};
DownloadEstimate EstimateDownload(const std::string& contentLog);

struct SteamState {
    fs::path SteamDir;
    bool SteamOffline = false;
};
SteamState FindSteam();

struct GameInstall {
    fs::path Root;      // ...\steamapps\common\TEKKEN 8
    fs::path Win64;     // ...\Polaris\Binaries\Win64
    fs::path Paks;      // ...\Polaris\Content\Paks
    std::string BuildId;
    uint64_t BytesDownloaded = 0, BytesToDownload = 0;
    int StateFlags = 0;
    bool Found = false;
    bool FullyInstalled() const { return Found && (StateFlags & 4) && !(StateFlags & 1024) && !(StateFlags & 2); }
};
GameInstall FindGame(const SteamState& steam);

// ----- Session files
fs::path DataDir(const GameInstall& game);      // <Win64>\T8Cine
fs::path SkinMountDir(const GameInstall& game); // <Paks>\~T8Cine

struct InstallResult {
    bool Ok = false;
    std::string Message;
};

// Copies the mod DLL in and writes the session file. A dinput8.dll from another mod is set aside first and
// put back by RemoveMod. skinFolders are copied so the game mounts them.
InstallResult InstallMod(const GameInstall& game, const fs::path& modDll, const std::vector<fs::path>& skinFolders);

// Takes everything back out. Safe to call at any time.
InstallResult RemoveMod(const GameInstall& game);

// Is the file at this path a T8Cine build? (Looks for our marker string.)
bool IsOurDll(const fs::path& path);
bool ModFilesPresent(const GameInstall& game);

// Pak mods and other mods' DLLs that T8Cine didn't put there. They load in every launch, online too, so the launcher warns
// about them. They belong to the user and are left alone.
std::vector<std::string> OtherMods(const GameInstall& game);

// ----- Skins: each folder under skinsDir holds one mod's .pak (plus .utoc and .ucas when it has them).
struct Skin {
    std::string Name;
    fs::path Folder;
    uint64_t Bytes = 0;
    int PakCount = 0;
};
std::vector<Skin> ListSkins(const fs::path& skinsDir);

// ----- Replay vault: copies of the game's save files, tagged with the game build they were made on.
fs::path SaveGamesDir();

// The game keeps each saved replay in its own encrypted replay_game_no*.sav, next to an index.
struct SavedReplay {
    std::string FileName;
    std::string Modified;
    uint64_t Bytes = 0;
};
std::vector<SavedReplay> ListSavedReplays(const fs::path& saveDir);

struct VaultEntry {
    std::string Name;
    fs::path Folder;
    std::string BuildId;
    std::string Created;
    uint64_t Bytes = 0;
    int Files = 0;
};
std::vector<VaultEntry> ListVault(const fs::path& vaultDir);
InstallResult SnapshotSaves(const fs::path& saveDir, const fs::path& vaultDir, const std::string& name, const std::string& buildId);

// Backs the live saves up into the vault first, then restores the chosen snapshot over them.
InstallResult RestoreSnapshot(const fs::path& saveDir, const fs::path& vaultDir, const VaultEntry& entry, const std::string& currentBuildId);

} // namespace files
