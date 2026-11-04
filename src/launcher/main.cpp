// tekken8mvm launcher. A cinematic session puts the mod and any chosen skins in the game folder, starts the game
// through Steam, and takes everything out again when the game closes. A normal launch makes sure none of it
// is there, so online play always runs on untouched files.
#include <d3d11.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <windows.h>

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "common/config.h"
#include "launcher/game_files.h"

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

bool GameProcessRunning()
{
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return false;
    PROCESSENTRY32W entry = { sizeof(entry) };
    bool found = false;
    for (BOOL more = Process32FirstW(snapshot, &entry); more; more = Process32NextW(snapshot, &entry))
        if (_wcsicmp(entry.szExeFile, files::GameExe) == 0) {
            found = true;
            break;
        }
    CloseHandle(snapshot);
    return found;
}

void OpenPath(const std::wstring& target)
{
    ShellExecuteW(nullptr, L"open", target.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

fs::path ModDllPath()
{
    return g_app.Home / "mod" / "dinput8.dll";
}

fs::path SkinsDir()
{
    return g_app.Home / "skins";
}

fs::path VaultDir()
{
    return g_app.Home / "replay-vault";
}

std::string FindFfmpeg()
{
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
    ImGui::TextWrapped("To send a replay to a friend, start a cinematic session and open the REPLAYS tab in the editor: pick a replay and press Export. "
                       "A file you received goes in the shared_replays folder next to your recordings, and Load puts it in the game's Downloaded Replays. "
                       "It stays there until you close the game. Both of you need the same game version, and the file holds both players' names.");
    if (ImGui::Button("Open shared replays")) {
        wchar_t profile[MAX_PATH] = L".";
        GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH);
        const fs::path folder = fs::path(profile) / "Videos" / "tekken8mvm" / "shared_replays";
        std::error_code ignored;
        fs::create_directories(folder, ignored);
        OpenPath(folder.wstring());
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
                           L"tekken8mvm", MB_YESNO | MB_ICONWARNING) != IDYES)
            return 0;
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, wchar_t*, int show)
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

    WNDCLASSEXW windowClass = { sizeof(windowClass) };
    windowClass.style = CS_CLASSDC;
    windowClass.lpfnWndProc = WndProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.lpszClassName = L"tekken8mvm";
    RegisterClassExW(&windowClass);

    const float scale = ImGui_ImplWin32_GetDpiScaleForMonitor(MonitorFromPoint(POINT { 0, 0 }, MONITOR_DEFAULTTOPRIMARY));
    HWND window = CreateWindowW(windowClass.lpszClassName, L"tekken8mvm", WS_OVERLAPPEDWINDOW, 100, 100, static_cast<int>(760 * scale),
                                static_cast<int>(640 * scale), nullptr, nullptr, instance, nullptr);
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
