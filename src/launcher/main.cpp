// tekken8mvm launcher. A cinematic session puts the mod and any chosen skins in the game folder, starts the game
// through Steam, and takes everything out again when the game closes. A normal launch makes sure none of it
// is there, so online play always runs on untouched files.
#include <d3d11.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <windows.h>
#include <commdlg.h>

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "common/config.h"
#include "launcher/game_files.h"
#include "launcher/updater.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace fs = std::filesystem;

namespace {

ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_context = nullptr;
IDXGISwapChain* g_swapChain = nullptr;
ID3D11RenderTargetView* g_target = nullptr;

enum class Session { Idle, WaitingForGame, GameRunning };

struct App {
    fs::path Home; // the folder the launcher lives in
    Ini Settings;
    files::SteamState Steam;
    files::GameInstall Game;
    Session State = Session::Idle;
    bool ModSession = false;
    std::chrono::steady_clock::time_point LaunchTime;
    std::string Status;
    bool StatusIsError = false;
    std::vector<files::Skin> Skins;
    std::vector<files::VaultEntry> Vault;
    std::vector<files::SavedReplay> Replays;
    std::vector<std::string> OtherMods;
    char SnapshotName[64] = "my-replays";
    int RestoreIndex = -1;
    double LastRefresh = -100;
    files::DownloadEstimate Download;
} g_app;

const ImVec4 Accent(0.90f, 0.20f, 0.25f, 1.0f);
const ImVec4 Dim(0.62f, 0.62f, 0.66f, 1.0f);
const ImVec4 Good(0.35f, 0.82f, 0.50f, 1.0f);
const ImVec4 Warn(0.95f, 0.72f, 0.30f, 1.0f);

void SetStatus(const std::string& text, bool error = false)
{
    g_app.Status = text;
    g_app.StatusIsError = error;
}

bool ProcessRunning(const wchar_t* exe)
{
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return false;
    PROCESSENTRY32W entry = { sizeof(entry) };
    bool found = false;
    for (BOOL more = Process32FirstW(snapshot, &entry); more; more = Process32NextW(snapshot, &entry))
        if (_wcsicmp(entry.szExeFile, exe) == 0) {
            found = true;
            break;
        }
    CloseHandle(snapshot);
    return found;
}

bool GameProcessRunning()
{
    return ProcessRunning(files::GameExe);
}

void OpenPath(const std::wstring& target)
{
    ShellExecuteW(nullptr, L"open", target.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

fs::path ModDllPath()
{
    return g_app.Home / "mod" / "dinput8.dll";
}

fs::path ReplaySaverPath()
{
    return g_app.Home / "replay-saver" / "dinput8.dll";
}

fs::path SkinsDir()
{
    return g_app.Home / "skins";
}

fs::path VaultDir()
{
    return g_app.Home / "replay-vault";
}

// The installer puts its own ffmpeg next to the launcher. Otherwise the one on PATH.
std::string FindFfmpeg()
{
    std::error_code ignored;
    const fs::path bundled = g_app.Home / "ffmpeg" / "bin" / "ffmpeg.exe";
    if (fs::exists(bundled, ignored))
        return bundled.string();
    wchar_t path[MAX_PATH];
    if (SearchPathW(nullptr, L"ffmpeg.exe", nullptr, MAX_PATH, path, nullptr))
        return fs::path(path).string();
    return "ffmpeg";
}

void WriteModSettings()
{
    const fs::path file = files::DataDir(g_app.Game) / "tekken8mvm.ini";
    Ini ini;
    ini.Load(file);
    ini.Set("ffmpeg_path", FindFfmpeg());
    if (ini.Get("online_guard").empty())
        ini.SetBool("online_guard", true);
    std::error_code ignored;
    fs::create_directories(file.parent_path(), ignored);
    ini.Save(file);
}

void RefreshLists()
{
    std::error_code ignored;
    fs::create_directories(SkinsDir(), ignored);
    fs::create_directories(VaultDir(), ignored);
    g_app.Skins = files::ListSkins(SkinsDir());
    g_app.Vault = files::ListVault(VaultDir());
    g_app.Replays = files::ListSavedReplays(files::SaveGamesDir());
}

void Refresh()
{
    g_app.Steam = files::FindSteam();
    g_app.Game = files::FindGame(g_app.Steam);
    if (g_app.Game.Found && !GameProcessRunning())
        files::MoveFromOldName(g_app.Game);
    if (wchar_t profile[MAX_PATH]; GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH) > 0) {
        std::error_code ignored;
        const fs::path videos = fs::path(profile) / "Videos";
        if (fs::exists(videos / "T8Cine", ignored) && !fs::exists(videos / "tekken8mvm", ignored))
            fs::rename(videos / "T8Cine", videos / "tekken8mvm", ignored);
    }
    if (g_app.Game.Found && !g_app.Game.FullyInstalled())
        g_app.Download = files::EstimateDownload(files::ReadTextFile(g_app.Steam.SteamDir / "logs" / "content_log.txt"));
    if (g_app.Game.Found)
        g_app.OtherMods = files::OtherMods(g_app.Game);
}

void UseGameSettings(const char* mode)
{
    files::SwitchGameSettings(files::DataDir(g_app.Game) / "game_settings", files::GameSettingsFile(), mode);
}

void LaunchCinematic()
{
    std::vector<fs::path> skins;
    for (const files::Skin& skin : g_app.Skins)
        if (g_app.Settings.GetBool("skin." + skin.Name, false))
            skins.push_back(skin.Folder);

    WriteModSettings();
    const files::InstallResult result = files::InstallMod(g_app.Game, ModDllPath(), skins);
    if (!result.Ok) {
        SetStatus(result.Message, true);
        files::RemoveMod(g_app.Game);
        return;
    }
    UseGameSettings("mvm");
    OpenPath(std::wstring(L"steam://rungameid/") + files::AppId);
    g_app.State = Session::WaitingForGame;
    g_app.ModSession = true;
    g_app.LaunchTime = std::chrono::steady_clock::now();
    SetStatus("Starting the game with the mod...");
}

void LaunchNormal()
{
    const files::InstallResult result = files::RemoveMod(g_app.Game);
    if (!result.Ok) {
        SetStatus(result.Message, true);
        return;
    }
    UseGameSettings("normal");
    OpenPath(std::wstring(L"steam://rungameid/") + files::AppId);
    g_app.State = Session::WaitingForGame;
    g_app.ModSession = false;
    g_app.LaunchTime = std::chrono::steady_clock::now();
    SetStatus("Starting the game with no mods...");
}

void EndSession()
{
    if (g_app.ModSession) {
        const files::InstallResult result = files::RemoveMod(g_app.Game);
        UseGameSettings("normal"); // starting the game from Steam next time is normal play
        SetStatus(result.Ok ? "Game closed. The mod and skins are out of the game folder." : result.Message, !result.Ok);
    } else {
        SetStatus("Game closed.");
    }
    g_app.State = Session::Idle;
    g_app.ModSession = false;
}

void UpdateSession()
{
    if (g_app.State == Session::WaitingForGame) {
        if (GameProcessRunning()) {
            g_app.State = Session::GameRunning;
            SetStatus(g_app.ModSession ? "Cinematic session running. Press Insert in the game for the menu." : "Game running with no mods.");
        } else if (std::chrono::steady_clock::now() - g_app.LaunchTime > std::chrono::minutes(5)) {
            SetStatus("The game didn't start within 5 minutes.", true);
            EndSession();
        }
    } else if (g_app.State == Session::GameRunning && !GameProcessRunning()) {
        EndSession();
    }
}

std::string Gigabytes(uint64_t bytes)
{
    char text[32];
    snprintf(text, sizeof(text), "%.1f GB", bytes / 1073741824.0);
    return text;
}

std::string Megabytes(uint64_t bytes)
{
    char text[32];
    snprintf(text, sizeof(text), "%.1f MB", bytes / 1048576.0);
    return text;
}

void PlayTab()
{
    const files::GameInstall& game = g_app.Game;

    ImGui::TextColored(Accent, "Status");
    ImGui::Separator();
    if (!game.Found) {
        ImGui::TextColored(Warn, "Tekken 8 isn't installed in any Steam library.");
    } else if (!game.FullyInstalled()) {
        const files::DownloadEstimate& estimate = g_app.Download;
        ImGui::TextColored(Warn, "Tekken 8 is still downloading");
        if (estimate.Known) {
            char overlay[64];
            snprintf(overlay, sizeof(overlay), "about %s of %s", Gigabytes(estimate.Done).c_str(), Gigabytes(estimate.Total).c_str());
            ImGui::ProgressBar(static_cast<float>(static_cast<double>(estimate.Done) / estimate.Total), ImVec2(-1, 0), overlay);
            ImGui::TextColored(Dim, "%.0f Mbps right now. Steam shows the exact progress.", estimate.Mbps);
        }
    } else {
        ImGui::TextColored(Good, "Tekken 8 installed (build %s)", game.BuildId.c_str());
    }
    if (g_app.Steam.SteamOffline)
        ImGui::TextColored(Good, "Steam is in Offline Mode");
    else
        ImGui::TextColored(Warn, "Steam is online");
    if (game.Found && files::ModFilesPresent(game) && g_app.State == Session::Idle)
        ImGui::TextColored(Warn, "Mod files are still in the game folder (a crash can leave them). A normal launch removes them.");

    ImGui::Spacing();
    ImGui::Spacing();
    ImGui::TextColored(Accent, "Cinematic session");
    ImGui::Separator();
    ImGui::TextWrapped("Free camera, slow motion, dolly paths, effect toggles, recording and your chosen skins. Offline only.");
    const bool canPlay = game.FullyInstalled() && g_app.State == Session::Idle;
    ImGui::BeginDisabled(!canPlay || !g_app.Steam.SteamOffline || !fs::exists(ModDllPath()));
    if (ImGui::Button("Start cinematic session", ImVec2(260, 40)))
        LaunchCinematic();
    ImGui::EndDisabled();
    if (!g_app.Steam.SteamOffline)
        ImGui::TextColored(Warn, "To enable this, choose Steam > Go Offline in Steam, then come back here.");
    if (!fs::exists(ModDllPath()))
        ImGui::TextColored(Accent, "Missing %s", ModDllPath().string().c_str());

    ImGui::Spacing();
    ImGui::Spacing();
    ImGui::TextColored(Accent, "Normal game");
    ImGui::Separator();
    ImGui::TextWrapped("tekken8mvm and its skins are taken out of the game folder first. Use this for online play.");
    if (!g_app.OtherMods.empty()) {
        std::string list;
        for (const std::string& mod : g_app.OtherMods)
            list += (list.empty() ? "" : ", ") + mod;
        ImGui::TextColored(Warn, "Other mods are still in the game folder and load in every launch, online too:");
        ImGui::TextWrapped("%s", list.c_str());
        ImGui::TextColored(Warn, "tekken8mvm didn't put these there and leaves them alone. Remove them yourself before playing online.");
    }
    if (g_app.Game.Found && files::ReplaySaverInstalled(g_app.Game))
        ImGui::TextColored(Dim, "The replay saver is in and stays in, online too: it saves your replays as you play. The Replays tab takes it out.");
    ImGui::BeginDisabled(!canPlay);
    if (ImGui::Button("Start normal game", ImVec2(260, 32)))
        LaunchNormal();
    ImGui::EndDisabled();

    if (g_app.State != Session::Idle) {
        ImGui::Spacing();
        ImGui::TextColored(Good, "%s", g_app.ModSession ? "Cinematic session in progress" : "Normal game in progress");
        ImGui::TextColored(Dim, "Keep this window open: it cleans up when the game closes.");
    }
}

void SkinsTab()
{
    ImGui::TextWrapped("Each folder inside the skins folder holds one mod's files (.pak, and .utoc and .ucas if it has them). "
                       "Ticked skins go into the game only for a cinematic session and come out when it closes.");
    if (ImGui::Button("Open skins folder"))
        OpenPath(SkinsDir().wstring());
    ImGui::SameLine();
    if (ImGui::Button("Refresh"))
        RefreshLists();
    ImGui::Separator();
    if (g_app.Skins.empty())
        ImGui::TextColored(Dim, "No skins yet. Put a folder with a mod's files in the skins folder.");
    for (const files::Skin& skin : g_app.Skins) {
        bool enabled = g_app.Settings.GetBool("skin." + skin.Name, false);
        char label[256];
        snprintf(label, sizeof(label), "%s   (%s)", skin.Name.c_str(), Megabytes(skin.Bytes).c_str());
        if (ImGui::Checkbox(label, &enabled)) {
            g_app.Settings.SetBool("skin." + skin.Name, enabled);
            g_app.Settings.Save(g_app.Home / "launcher.ini");
        }
        if (skin.PakCount == 0) {
            ImGui::SameLine();
            ImGui::TextColored(Warn, "no .pak inside");
        }
    }
}

// Where shared replay files go: the shared_replays folder next to the recordings, as the mod has it.
fs::path SharedReplaysDir()
{
    if (g_app.Game.Found) {
        Ini ini;
        ini.Load(files::DataDir(g_app.Game) / "tekken8mvm.ini");
        const std::string output = ini.Get("output_dir");
        if (!output.empty())
            return fs::path(output) / "shared_replays";
    }
    wchar_t profile[MAX_PATH] = L".";
    GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH);
    return fs::path(profile) / "Videos" / "tekken8mvm" / "shared_replays";
}

std::vector<std::string> SharedReplayNames()
{
    std::vector<std::string> names;
    std::error_code ignored;
    for (const auto& entry : fs::directory_iterator(SharedReplaysDir(), ignored))
        if (entry.path().extension() == ".t8replay") {
            const std::u8string stem = entry.path().stem().u8string();
            names.emplace_back(stem.begin(), stem.end());
        }
    std::sort(names.begin(), names.end());
    return names;
}

std::vector<fs::path> PickReplayFiles()
{
    std::vector<wchar_t> buffer(32768, 0);
    OPENFILENAMEW dialog = { sizeof(dialog) };
    dialog.hwndOwner = GetActiveWindow();
    dialog.lpstrFilter = L"Replay files (*.t8replay)\0*.t8replay\0";
    dialog.lpstrFile = buffer.data();
    dialog.nMaxFile = static_cast<DWORD>(buffer.size());
    dialog.lpstrTitle = L"Add replay files";
    dialog.Flags = OFN_ALLOWMULTISELECT | OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    std::vector<fs::path> picked;
    if (!GetOpenFileNameW(&dialog))
        return picked;
    // One file: its full path. Several: the folder, then each name, each ending in a zero, and an extra zero at the end.
    const fs::path first = buffer.data();
    const wchar_t* next = buffer.data() + wcslen(buffer.data()) + 1;
    if (!*next)
        picked.push_back(first);
    for (; *next; next += wcslen(next) + 1)
        picked.push_back(first / next);
    return picked;
}

// Copies the files into the shared folder, where the mod loads them when the game starts.
void AddReplayFiles(const std::vector<fs::path>& paths)
{
    if (paths.empty())
        return;
    const fs::path folder = SharedReplaysDir();
    std::error_code error;
    fs::create_directories(folder, error);
    int added = 0, skipped = 0;
    for (const fs::path& path : paths) {
        if (path.extension() != ".t8replay") {
            ++skipped;
            continue;
        }
        if (fs::equivalent(path.parent_path(), folder, error)) {
            ++added; // already there
            continue;
        }
        error.clear();
        fs::copy_file(path, folder / path.filename(), fs::copy_options::overwrite_existing, error);
        error ? ++skipped : ++added;
    }
    std::string text = std::to_string(added) + (added == 1 ? " replay file added." : " replay files added.");
    if (skipped)
        text += " " + std::to_string(skipped) + " couldn't be added (only .t8replay files can).";
    if (added)
        text += g_app.State == Session::Idle ? " They go into the game's Downloaded Replays when you start a session." : " The game picks them up within a few seconds.";
    SetStatus(text, added == 0);
}

void ReplaysTab()
{
    const fs::path saves = files::SaveGamesDir();

    ImGui::TextColored(Accent, "Your saved replays");
    ImGui::Separator();
    if (g_app.Replays.empty()) {
        ImGui::TextColored(Dim, "None found yet. Save a replay in the game and it shows up here.");
    } else {
        ImGui::Text("%d replays, newest %s", static_cast<int>(g_app.Replays.size()), g_app.Replays.front().Modified.c_str());
    }
    ImGui::Spacing();
    ImGui::TextColored(Accent, "Replays from friends");
    ImGui::Separator();
    ImGui::TextWrapped("Add the .t8replay files a friend sent you, here or by dropping them on this window. When you start a session they go into the "
                       "game's Replay > Downloaded Replays by themselves, a few seconds after the game has loaded its own. One that's already there is skipped. "
                       "To send one, open the editor's Replays tab in a session and press Export, or choose Export Replay in the game's own replay box.");
    if (ImGui::Button("Add replay files..."))
        AddReplayFiles(PickReplayFiles());
    ImGui::SameLine();
    if (ImGui::Button("Open the folder")) {
        const fs::path folder = SharedReplaysDir();
        std::error_code ignored;
        fs::create_directories(folder, ignored);
        OpenPath(folder.wstring());
    }
    if (g_app.Game.Found) {
        static int autoLoad = -1, autoExport = -1;
        const fs::path ini = files::DataDir(g_app.Game) / "tekken8mvm.ini";
        Ini settings;
        if (autoLoad < 0) {
            settings.Load(ini);
            autoLoad = settings.GetBool("auto_load_replays", true) ? 1 : 0;
            autoExport = settings.GetBool("auto_export_replays", true) ? 1 : 0;
        }
        bool on = autoLoad == 1;
        if (ImGui::Checkbox("Load them into the game on their own", &on)) {
            autoLoad = on ? 1 : 0;
            settings.Load(ini);
            settings.SetBool("auto_load_replays", on);
            std::error_code ignored;
            fs::create_directories(ini.parent_path(), ignored);
            settings.Save(ini);
        }
        if (!on)
            ImGui::TextColored(Dim, "Off: load each one with Load in the editor's Replays tab.");
        bool exportOn = autoExport == 1;
        if (ImGui::Checkbox("Export my replays to .t8replay files on their own", &exportOn)) {
            autoExport = exportOn ? 1 : 0;
            settings.Load(ini);
            settings.SetBool("auto_export_replays", exportOn);
            std::error_code ignored;
            fs::create_directories(ini.parent_path(), ignored);
            settings.Save(ini);
        }
        ImGui::TextColored(Dim, "In a cinematic session each replay is saved a few seconds after the game adds it, into my_replays.");

        // The replay saver: the same saving while you play normally, online included.
        static int saver = -1;
        if (saver < 0)
            saver = files::ReplaySaverInstalled(g_app.Game) ? 1 : 0;
        bool saverOn = saver == 1;
        ImGui::BeginDisabled(GameProcessRunning() || !fs::exists(ReplaySaverPath()));
        if (ImGui::Checkbox("Save my replays while I play, online too", &saverOn)) {
            const files::InstallResult result = saverOn ? files::InstallReplaySaver(g_app.Game, ReplaySaverPath()) : files::RemoveReplaySaver(g_app.Game);
            SetStatus(result.Message, !result.Ok);
            saver = files::ReplaySaverInstalled(g_app.Game) ? 1 : 0;
        }
        ImGui::EndDisabled();
        ImGui::TextWrapped("Puts a small mod (dinput8.dll) in the game folder that stays there, like a pak mod. While you play, ranked included, it saves "
                           "each of your replays into my_replays a few seconds after the game adds it to its list. It only reads the replay list and "
                           "writes files: no hooks, nothing changed in the game.");
        ImGui::TextColored(Warn, "Bandai Namco's ban reasons include unauthorized mods. Using it online is your call and your account's risk.");
        if (GameProcessRunning())
            ImGui::TextColored(Dim, "Close the game to put it in or take it out.");
    }
    const std::vector<std::string> waiting = SharedReplayNames();
    if (waiting.empty()) {
        ImGui::TextColored(Dim, "No replay files yet.");
    } else {
        ImGui::Text("%d replay file%s:", static_cast<int>(waiting.size()), waiting.size() == 1 ? "" : "s");
        if (ImGui::BeginChild("sharedlist", ImVec2(0, ImGui::GetTextLineHeightWithSpacing() * std::min<float>(5.5f, static_cast<float>(waiting.size()) + 0.5f)),
                              ImGuiChildFlags_Borders)) {
            for (const std::string& name : waiting)
                ImGui::TextUnformatted(name.c_str());
        }
        ImGui::EndChild();
    }

    ImGui::Spacing();
    ImGui::Spacing();
    ImGui::TextColored(Accent, "Replay vault");
    ImGui::Separator();
    ImGui::TextWrapped("A snapshot is a full copy of your own save folder, for keeping replays safe on this PC. It isn't for sharing. "
                       "Game patches can stop old replays from playing, so each snapshot remembers its game build.");
    ImGui::TextColored(Dim, "Saves: %s", saves.string().c_str());
    const bool idle = g_app.State == Session::Idle && !GameProcessRunning();
    ImGui::BeginDisabled(!idle);
    ImGui::SetNextItemWidth(220);
    ImGui::InputText("##name", g_app.SnapshotName, sizeof(g_app.SnapshotName));
    ImGui::SameLine();
    if (ImGui::Button("Save snapshot")) {
        const auto result = files::SnapshotSaves(saves, VaultDir(), g_app.SnapshotName, g_app.Game.BuildId);
        SetStatus(result.Message, !result.Ok);
        RefreshLists();
    }
    ImGui::EndDisabled();
    if (!idle)
        ImGui::TextColored(Warn, "Close the game before saving or restoring a snapshot.");

    if (ImGui::BeginTable("vault", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY, ImVec2(0, 180))) {
        ImGui::TableSetupColumn("Name");
        ImGui::TableSetupColumn("Created", ImGuiTableColumnFlags_WidthFixed, 130);
        ImGui::TableSetupColumn("Build", ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableHeadersRow();
        for (int i = 0; i < static_cast<int>(g_app.Vault.size()); ++i) {
            const files::VaultEntry& entry = g_app.Vault[i];
            ImGui::PushID(i);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(entry.Name.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(entry.Created.c_str());
            ImGui::TableNextColumn();
            const bool sameBuild = entry.BuildId == g_app.Game.BuildId;
            ImGui::TextColored(sameBuild ? Good : Warn, "%s", entry.BuildId.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(Megabytes(entry.Bytes).c_str());
            ImGui::TableNextColumn();
            ImGui::BeginDisabled(!idle);
            if (ImGui::SmallButton("Restore"))
                g_app.RestoreIndex = i;
            ImGui::EndDisabled();
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    if (g_app.RestoreIndex >= 0) {
        ImGui::OpenPopup("Restore snapshot?");
    }
    if (ImGui::BeginPopupModal("Restore snapshot?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        if (g_app.RestoreIndex >= 0 && g_app.RestoreIndex < static_cast<int>(g_app.Vault.size())) {
            ImGui::Text("Replace your current saves with \"%s\"?", g_app.Vault[g_app.RestoreIndex].Name.c_str());
            ImGui::TextColored(Dim, "Your current saves get backed up to the vault first.");
            ImGui::TextColored(Warn, "Steam Cloud syncs these files, so the restored saves can replace your cloud copy too.");
            if (ImGui::Button("Restore", ImVec2(120, 0))) {
                const auto result = files::RestoreSnapshot(saves, VaultDir(), g_app.Vault[g_app.RestoreIndex], g_app.Game.BuildId);
                SetStatus(result.Message, !result.Ok);
                g_app.RestoreIndex = -1;
                RefreshLists();
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
        }
        if (ImGui::Button("Cancel", ImVec2(120, 0))) {
            g_app.RestoreIndex = -1;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

// ----- Tekken Tag Tournament 2, on the BlueSpark recompilation. The studio there is linked into a separate copy of the game on this
// PC (BlueSpark's own game and its files stay as they are), so "install" means building that copy with the compiler BlueSpark's own
// Install already downloaded. The scripts and the studio's sources are in the ttt2 folder next to this launcher.

// The BlueSpark release this studio is made for: it names things inside BlueSpark's prebuilt library, which move between releases.
constexpr int Ttt2Release = 23;

fs::path Ttt2Scripts()
{
    std::error_code ignored;
    for (const fs::path& candidate : { g_app.Home / "ttt2", g_app.Home.parent_path() / "ttt2" }) // installed, or run from a build
        if (fs::exists(candidate / "build-studio.ps1", ignored))
            return candidate;
    return {};
}

// What the play test folder holds: its release number, 0 if it isn't a built BlueSpark play test.
int Ttt2PlaytestRelease(const fs::path& folder)
{
    std::error_code ignored;
    if (folder.empty() || !fs::exists(folder / "rex" / "out" / "build" / "win-amd64-release" / "ttt2.exe", ignored))
        return 0;
    std::ifstream info(folder / "rex" / "package-info.txt");
    std::string line;
    while (std::getline(info, line))
        if (line.rfind("patch=", 0) == 0)
            return atoi(line.c_str() + 6);
    return 0;
}

void RunTtt2Script(const fs::path& script, const std::wstring& arguments, bool show)
{
    const std::wstring command = L"-NoProfile -ExecutionPolicy Bypass " + std::wstring(show ? L"-NoExit " : L"") + L"-File \"" + script.wstring() + L"\" " + arguments;
    ShellExecuteW(nullptr, L"open", L"powershell.exe", command.c_str(), script.parent_path().c_str(), show ? SW_SHOWNORMAL : SW_HIDE);
}

void Ttt2Tab()
{
    static char folder[MAX_PATH] = "";
    static bool loaded = false;
    if (!loaded) {
        loaded = true;
        strncpy_s(folder, g_app.Settings.Get("ttt2_playtest").c_str(), _TRUNCATE);
    }
    ImGui::TextColored(Accent, "Tekken Tag Tournament 2 (BlueSpark)");
    ImGui::Separator();
    ImGui::TextWrapped("Tekken Studio for TTT2 runs on BlueSpark, the fan-made native port of the Xbox 360 game (github.com/avim2/BlueSpark). BlueSpark records every "
                       "offline Versus match and every online match by itself. The studio is a separate, offline-only copy of the game on your PC: BlueSpark's own "
                       "game, your online account and its files are not touched.");
    ImGui::Spacing();
    ImGui::TextUnformatted("Your BlueSpark play test folder (the one with Start Here in it):");
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - ImGui::GetFontSize() * 6);
    if (ImGui::InputText("##ttt2folder", folder, sizeof(folder))) {
        g_app.Settings.Set("ttt2_playtest", folder);
        g_app.Settings.Save(g_app.Home / "launcher.ini");
    }
    ImGui::SameLine();
    if (ImGui::Button("Open##ttt2") && folder[0])
        OpenPath(fs::path(folder).wstring());

    const fs::path playtest = folder;
    const fs::path scripts = Ttt2Scripts();
    const int release = Ttt2PlaytestRelease(playtest);
    std::error_code ignored;
    const fs::path studioExe = playtest / "rex" / "out" / "build" / "tekken-studio" / "ttt2-studio.exe";
    const bool built = release != 0 && fs::exists(studioExe, ignored);
    const bool running = ProcessRunning(L"ttt2-studio.exe") || ProcessRunning(L"ttt2.exe");

    if (scripts.empty()) {
        ImGui::TextColored(Warn, "The studio's TTT2 files (the ttt2 folder) aren't next to this launcher. Install Tekken Studio again.");
        return;
    }
    if (release == 0) {
        ImGui::TextColored(Warn, folder[0] ? "That folder isn't a BlueSpark play test that has been built. Open its Start Here and run Install first."
                                           : "Paste the folder's path above. BlueSpark must be installed and built once with its Start Here.");
        return;
    }
    if (release != Ttt2Release) {
        ImGui::TextColored(Warn, "This BlueSpark is release %d, and this Tekken Studio is made for release %d. Update one so they match.", release, Ttt2Release);
        return;
    }
    ImGui::TextColored(Good, built ? "BlueSpark release %d found, and the studio is installed." : "BlueSpark release %d found. The studio isn't installed yet.", release);

    ImGui::Spacing();
    if (ImGui::Button(built ? "Install again (after an update)" : "Install the studio", ImVec2(ImGui::GetFontSize() * 16, 0))) {
        RunTtt2Script(scripts / "build-studio.ps1", L"-Playtest \"" + playtest.wstring() + L"\"", true);
        SetStatus("Building the studio in a window of its own: it takes under a minute. Close that window when it says Built.");
    }
    ImGui::SameLine();
    ImGui::TextDisabled("links the studio into a copy of the game, about 90 MB");

    ImGui::Spacing();
    ImGui::BeginDisabled(!built || running);
    if (ImGui::Button("Watch replays (simple)", ImVec2(ImGui::GetFontSize() * 16, ImGui::GetFontSize() * 2.2f))) {
        RunTtt2Script(scripts / "run-studio.ps1", L"-Mode simple -Playtest \"" + playtest.wstring() + L"\"", false);
        SetStatus("TTT2 is starting. Its REPLAYS tab lists your matches: A watches one.");
    }
    ImGui::SameLine();
    if (ImGui::Button("Open the studio", ImVec2(ImGui::GetFontSize() * 16, ImGui::GetFontSize() * 2.2f))) {
        RunTtt2Script(scripts / "run-studio.ps1", L"-Mode studio -Playtest \"" + playtest.wstring() + L"\"", false);
        SetStatus("TTT2 is starting with the studio open. Insert closes and opens it.");
    }
    ImGui::EndDisabled();
    if (running)
        ImGui::TextColored(Dim, "TTT2 is running. Close it before starting the studio: they share your saves.");
    ImGui::Spacing();
    ImGui::TextWrapped("Simple is for watching: BlueSpark's own replay viewer with the studio's playback bar. Studio adds the editor: a free camera, camera paths "
                       "on a timeline, and pause, slow motion and frame steps in Practice and offline fights. Inside a replay the camera works, and time is "
                       "BlueSpark's own: A pauses, LB goes back 3 seconds. Insert switches between them at any time, and Home hides the bar.");
    ImGui::TextColored(Dim, "The studio never goes online: no sign-in, no matchmaking, no uploads. Play online with BlueSpark's own Start Here.");
}

void HelpTab()
{
    ImGui::TextColored(Accent, "Keys in the game");
    ImGui::Separator();
    const char* const rows[][2] = {
        { "Insert", "Menu" },         { "F5", "Free camera" },   { "F6", "Play dolly path" },
        { "F7", "Record" },           { "F8", "Pause" },         { ".", "Next frame" },
        { "[  ]", "Slower / faster" }, { "F9", "Hide the HUD" }, { "K", "Add dolly key (free camera on)" },
    };
    if (ImGui::BeginTable("keys", 2)) {
        ImGui::TableSetupColumn("key", ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableSetupColumn("what");
        for (const auto& row : rows) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(row[0]);
            ImGui::TableNextColumn();
            ImGui::TextColored(Dim, "%s", row[1]);
        }
        ImGui::EndTable();
    }
    ImGui::Spacing();
    ImGui::TextColored(Accent, "Staying safe");
    ImGui::Separator();
    ImGui::TextWrapped("Bandai Namco's rules allow bans for unauthorized mods, and no mod can promise otherwise. tekken8mvm keeps mods off "
                       "online play instead of hiding them: it only runs with Steam in Offline Mode, stops the game's internet connections "
                       "while it runs, turns itself off if Steam goes online, and leaves the game folder clean when it closes. "
                       "Don't copy skins or the mod into the game folder by hand.");
    ImGui::Spacing();
    if (g_app.Game.Found && ImGui::Button("Open the mod's log folder"))
        OpenPath(files::DataDir(g_app.Game).wstring());
    ImGui::SameLine();
    if (ImGui::Button("Open recordings")) {
        wchar_t profile[MAX_PATH] = L".";
        GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH);
        const fs::path videos = fs::path(profile) / "Videos" / "tekken8mvm";
        std::error_code ignored;
        fs::create_directories(videos, ignored);
        OpenPath(videos.wstring());
    }
}

// Updates: GitHub is asked once, when the tool opens.
updater::Release g_update;
std::atomic<int> g_updateState { 0 }; // 0 not asked, 1 asking, 2 asked, 3 downloading, 4 started
std::string g_updateProblem;
std::mutex g_updateMutex;

DWORD WINAPI UpdateCheck(void*)
{
    const updater::Release latest = updater::Latest();
    {
        std::lock_guard lock(g_updateMutex);
        if (latest.Found && updater::Newer(latest.Version, updater::CurrentVersion()))
            g_update = latest;
    }
    g_updateState = 2;
    return 0;
}

DWORD WINAPI UpdateDownload(void*)
{
    updater::Release release;
    {
        std::lock_guard lock(g_updateMutex);
        release = g_update;
    }
    const std::string problem = updater::DownloadAndRun(release);
    std::lock_guard lock(g_updateMutex);
    g_updateProblem = problem;
    g_updateState = problem.empty() ? 4 : 2;
    return 0;
}

void UpdateBanner()
{
    if (g_updateState == 0) {
        g_updateState = 1;
        CloseHandle(CreateThread(nullptr, 0, UpdateCheck, nullptr, 0, nullptr));
    }
    if (g_updateState < 2)
        return;
    std::lock_guard lock(g_updateMutex);
    if (g_update.Version.empty())
        return;
    if (g_updateState == 4) {
        PostQuitMessage(0); // the installer is running; close so it can replace this tool
        return;
    }
    ImGui::TextColored(Accent, "tekken8mvm %s is out (you have %s).", g_update.Version.c_str(), updater::CurrentVersion().c_str());
    ImGui::SameLine();
    if (g_updateState == 3) {
        ImGui::TextDisabled("Downloading. The tool closes, updates and opens again by itself.");
    } else if (ImGui::SmallButton("Update now")) {
        g_updateState = 3;
        g_updateProblem.clear();
        CloseHandle(CreateThread(nullptr, 0, UpdateDownload, nullptr, 0, nullptr));
    }
    if (!g_updateProblem.empty())
        ImGui::TextColored(ImVec4(1, 0.45f, 0.4f, 1), "%s", g_updateProblem.c_str());
    ImGui::Separator();
}

void DrawUi(double now)
{
    if (now - g_app.LastRefresh > 2.0) {
        g_app.LastRefresh = now;
        Refresh();
        UpdateSession();
    }

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->Pos);
    ImGui::SetNextWindowSize(viewport->Size);
    ImGui::Begin("##main", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus);
    UpdateBanner();
    if (ImGui::BeginTabBar("tabs")) {
        if (ImGui::BeginTabItem("Play")) {
            PlayTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Skins")) {
            SkinsTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Replays")) {
            ReplaysTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("TTT2")) {
            Ttt2Tab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Help")) {
            HelpTab();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    if (!g_app.Status.empty()) {
        ImGui::SetCursorPosY(ImGui::GetWindowHeight() - ImGui::GetFrameHeightWithSpacing() - 4);
        ImGui::Separator();
        ImGui::TextColored(g_app.StatusIsError ? Accent : Dim, "%s", g_app.Status.c_str());
    }
    ImGui::End();
}

void ApplyStyle()
{
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 0;
    style.FrameRounding = 4;
    style.GrabRounding = 4;
    style.TabRounding = 4;
    style.FramePadding = ImVec2(8, 5);
    style.ItemSpacing = ImVec2(8, 7);
    style.WindowPadding = ImVec2(14, 12);
    ImVec4* c = style.Colors;
    c[ImGuiCol_WindowBg] = ImVec4(0.07f, 0.07f, 0.09f, 1);
    c[ImGuiCol_PopupBg] = ImVec4(0.10f, 0.10f, 0.13f, 1);
    c[ImGuiCol_Border] = ImVec4(0.25f, 0.25f, 0.30f, 0.6f);
    c[ImGuiCol_FrameBg] = ImVec4(0.15f, 0.15f, 0.19f, 1);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.21f, 0.21f, 0.26f, 1);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.26f, 0.26f, 0.32f, 1);
    c[ImGuiCol_Button] = ImVec4(0.19f, 0.19f, 0.24f, 1);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.62f, 0.15f, 0.19f, 1);
    c[ImGuiCol_ButtonActive] = ImVec4(0.78f, 0.17f, 0.22f, 1);
    c[ImGuiCol_Tab] = ImVec4(0.13f, 0.13f, 0.17f, 1);
    c[ImGuiCol_TabHovered] = ImVec4(0.62f, 0.15f, 0.19f, 1);
    c[ImGuiCol_TabSelected] = ImVec4(0.78f, 0.17f, 0.22f, 1);
    c[ImGuiCol_CheckMark] = ImVec4(0.95f, 0.25f, 0.30f, 1);
    c[ImGuiCol_PlotHistogram] = ImVec4(0.78f, 0.17f, 0.22f, 1);
    c[ImGuiCol_TitleBgActive] = ImVec4(0.13f, 0.13f, 0.17f, 1);
}

void CreateTarget()
{
    ID3D11Texture2D* buffer = nullptr;
    g_swapChain->GetBuffer(0, IID_PPV_ARGS(&buffer));
    g_device->CreateRenderTargetView(buffer, nullptr, &g_target);
    buffer->Release();
}

void SaveBackBuffer(const wchar_t* path)
{
    ID3D11Texture2D* buffer = nullptr;
    g_swapChain->GetBuffer(0, IID_PPV_ARGS(&buffer));
    D3D11_TEXTURE2D_DESC desc;
    buffer->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* staging = nullptr;
    if (SUCCEEDED(g_device->CreateTexture2D(&desc, nullptr, &staging))) {
        g_context->CopyResource(staging, buffer);
        D3D11_MAPPED_SUBRESOURCE mapped;
        if (SUCCEEDED(g_context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped))) {
            std::ofstream file(path, std::ios::binary | std::ios::trunc);
            file << desc.Width << ' ' << desc.Height << '\n';
            for (UINT y = 0; y < desc.Height; ++y)
                file.write(static_cast<const char*>(mapped.pData) + y * mapped.RowPitch, desc.Width * 4);
            g_context->Unmap(staging, 0);
        }
        staging->Release();
    }
    buffer->Release();
}

LRESULT WINAPI WndProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (ImGui_ImplWin32_WndProcHandler(window, message, wParam, lParam))
        return 1;
    switch (message) {
    case WM_SIZE:
        if (g_device && wParam != SIZE_MINIMIZED) {
            if (g_target) {
                g_target->Release();
                g_target = nullptr;
            }
            g_swapChain->ResizeBuffers(0, LOWORD(lParam), HIWORD(lParam), DXGI_FORMAT_UNKNOWN, 0);
            CreateTarget();
        }
        return 0;
    case WM_CLOSE:
        if (g_app.State != Session::Idle && g_app.ModSession
            && MessageBoxW(window, L"A cinematic session is running. If you close this now, the mod stays in the game folder until the next launch.\n\nClose anyway?",
                           L"Tekken Studio", MB_YESNO | MB_ICONWARNING) != IDYES)
            return 0;
        break;
    case WM_DROPFILES: {
        const auto drop = reinterpret_cast<HDROP>(wParam);
        std::vector<fs::path> paths;
        const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        for (UINT i = 0; i < count; ++i) {
            wchar_t path[MAX_PATH * 4];
            if (DragQueryFileW(drop, i, path, static_cast<UINT>(std::size(path))))
                paths.emplace_back(path);
        }
        DragFinish(drop);
        AddReplayFiles(paths);
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, wchar_t* commandLine, int show)
{
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    g_app.Home = fs::path(self).parent_path();
    g_app.Settings.Load(g_app.Home / "launcher.ini");
    RefreshLists();

    // Clear up after a crash or a closed launcher, unless the game is running right now.
    g_app.Steam = files::FindSteam();
    g_app.Game = files::FindGame(g_app.Steam);
    if (g_app.Game.Found && !GameProcessRunning())
        files::MoveFromOldName(g_app.Game);
    if (wchar_t profile[MAX_PATH]; GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH) > 0) {
        std::error_code ignored;
        const fs::path videos = fs::path(profile) / "Videos";
        if (fs::exists(videos / "T8Cine", ignored) && !fs::exists(videos / "tekken8mvm", ignored))
            fs::rename(videos / "T8Cine", videos / "tekken8mvm", ignored);
    }
    if (GameProcessRunning()) {
        g_app.State = Session::GameRunning;
        g_app.ModSession = g_app.Game.Found && files::ModFilesPresent(g_app.Game);
    } else if (g_app.Game.Found && files::ModFilesPresent(g_app.Game)) {
        const auto result = files::RemoveMod(g_app.Game);
        SetStatus(result.Ok ? "Cleaned up mod files left from the last session." : result.Message, !result.Ok);
    }
    if (g_app.Game.Found && !GameProcessRunning())
        UseGameSettings("normal"); // in case a session ended with nothing here to put them back

    // An installed replay saver gets this version's copy.
    if (g_app.Game.Found && !GameProcessRunning() && files::ReplaySaverInstalled(g_app.Game) && fs::exists(ReplaySaverPath()))
        files::InstallReplaySaver(g_app.Game, ReplaySaverPath());

    // The uninstaller runs this first, so nothing of the mod is left in the game folder once the launcher is gone.
    if (commandLine && wcsstr(commandLine, L"--cleanup")) {
        if (g_app.Game.Found && !GameProcessRunning())
            files::RemoveReplaySaver(g_app.Game);
        return GameProcessRunning() ? 1 : 0;
    }

    WNDCLASSEXW windowClass = { sizeof(windowClass) };
    windowClass.style = CS_CLASSDC;
    windowClass.lpfnWndProc = WndProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1)); // the logo, from src/launcher/icon.rc
    windowClass.hIconSm = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                                        GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
    windowClass.lpszClassName = L"tekken8mvm";
    RegisterClassExW(&windowClass);

    const float scale = ImGui_ImplWin32_GetDpiScaleForMonitor(MonitorFromPoint(POINT { 0, 0 }, MONITOR_DEFAULTTOPRIMARY));
    HWND window = CreateWindowW(windowClass.lpszClassName, L"Tekken Studio v0.1 beta", WS_OVERLAPPEDWINDOW, 100, 100, static_cast<int>(760 * scale),
                                static_cast<int>(640 * scale), nullptr, nullptr, instance, nullptr);
    DragAcceptFiles(window, TRUE); // .t8replay files dropped on the window go into the shared folder
    BOOL dark = TRUE;
    DwmSetWindowAttribute(window, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof(dark));

    DXGI_SWAP_CHAIN_DESC desc = {};
    desc.BufferCount = 2;
    desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.OutputWindow = window;
    desc.SampleDesc.Count = 1;
    desc.Windowed = TRUE;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    D3D_FEATURE_LEVEL level;
    if (FAILED(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &desc, &g_swapChain,
                                             &g_device, &level, &g_context)))
        return 1;
    CreateTarget();
    ShowWindow(window, show);
    UpdateWindow(window);

    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    if (!io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 18.0f * scale))
        io.Fonts->AddFontDefault();
    ApplyStyle();
    ImGui::GetStyle().ScaleAllSizes(scale);
    ImGui_ImplWin32_Init(window);
    ImGui_ImplDX11_Init(g_device, g_context);

    wchar_t autoShot[MAX_PATH] = L"";
    GetEnvironmentVariableW(L"TEKKEN8MVM_LAUNCHER_SMOKE", autoShot, MAX_PATH);
    int frames = 0;

    bool running = true;
    while (running) {
        MSG message;
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
            if (message.message == WM_QUIT)
                running = false;
        }
        if (!running)
            break;

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        DrawUi(ImGui::GetTime());
        ImGui::Render();
        const float clear[4] = { 0.07f, 0.07f, 0.09f, 1 };
        g_context->OMSetRenderTargets(1, &g_target, nullptr);
        g_context->ClearRenderTargetView(g_target, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        // Test hook: draw for a moment, write the picture out and exit. This proves the launcher starts and
        // reads the real Steam files, with nothing clicked and nothing changed in the game folder.
        if (autoShot[0] && ++frames > 150) {
            SaveBackBuffer(autoShot);
            break;
        }
        g_swapChain->Present(1, 0);
    }

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    return 0;
}
