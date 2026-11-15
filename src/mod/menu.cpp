// The editor (Insert), laid out like IWXMVM: a menu bar on top, the game picture scaled down on the left, tool tabs on
// the right, and across the bottom the playback controls and the keyframe timeline.
#include <imgui.h>

#include <IconsFontAwesome6.h>

#include <windows.h>

#include <shellapi.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "common/config.h"
#include "mod/actors.h"
#include "mod/audio.h"
#include "mod/camera.h"
#include "mod/depth.h"
#include "mod/dvars.h"
#include "mod/effects.h"
#include "mod/game.h"
#include "mod/gameui.h"
#include "mod/guard.h"
#include "mod/lights.h"
#include "mod/log.h"
#include "mod/overlay.h"
#include "mod/postfx.h"
#include "mod/pakmods.h"
#include "mod/presets.h"
#include "mod/scene.h"
#include "mod/recorder.h"
#include "mod/replay.h"
#include "mod/replaydisplay.h"
#include "mod/reshadelink.h"
#include "mod/sharing.h"
#include "mod/sky.h"
#include "mod/spawner.h"
#include "mod/switcher.h"
#include "mod/window.h"

namespace menu {

namespace {

// The look is Iris Studio's (github.com/yerbisonline/IrisStudio, GPL-3.0): its Mono palette, neutral studio greys with a light grey
// for what's selected, mapped onto the editor the way its UI/Theme.cpp maps it, and its layout of tabs, rows and bars.
namespace pal {
constexpr uint32_t Bg = 0x1C1C1C;      // window backgrounds
constexpr uint32_t Surface = 0x242424; // panels and frames
constexpr uint32_t Raised = 0x2E2E2E;  // buttons, headers, tabs
constexpr uint32_t Border = 0x383838;
constexpr uint32_t Text = 0xD6D6D6;
constexpr uint32_t Muted = 0x8C8C8C;   // hints and what's switched off
constexpr uint32_t Accent = 0xCFCFCF;  // sliders, check marks, the tab that's open
constexpr uint32_t Ok = 0x7FB58F;
constexpr uint32_t Warning = 0xD4AA55;
constexpr uint32_t Bad = 0xD46A62;
constexpr uint32_t Blue = 0x3F9BD1;    // bars on the timeline
constexpr uint32_t Red = 0xD9453F;     // the recording range
} // namespace pal

constexpr ImU32 Hex(uint32_t rgb, int alpha = 255)
{
    return IM_COL32((rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255, alpha);
}

ImVec4 Colour(uint32_t rgb, float alpha = 1.0f)
{
    return { ((rgb >> 16) & 255) / 255.0f, ((rgb >> 8) & 255) / 255.0f, (rgb & 255) / 255.0f, alpha };
}

// One colour moved towards another by an amount from 0 to 1.
constexpr uint32_t Mix(uint32_t from, uint32_t to, float amount)
{
    const auto channel = [amount](uint32_t a, uint32_t b) { return static_cast<uint32_t>(a + (static_cast<float>(b) - a) * amount + 0.5f); };
    return channel((from >> 16) & 255, (to >> 16) & 255) << 16 | channel((from >> 8) & 255, (to >> 8) & 255) << 8 | channel(from & 255, to & 255);
}

const ImVec4 Accent = Colour(pal::Bad); // what needs looking at: a recording running, something that went wrong
const ImVec4 Dim = Colour(pal::Muted);
const ImVec4 Good = Colour(pal::Ok);
const ImVec4 Warn = Colour(pal::Warning);
constexpr double MinSpeed = 0.01, MaxSpeed = 4.0;

int g_tab = 0;
char g_notice[160] = "";

void Notice(const char* text)
{
    strncpy_s(g_notice, text, _TRUNCATE);
}

// The semi-bold face, for the clock and the names of the timeline's lanes.
ImFont* BoldFont()
{
    const ImGuiIO& io = ImGui::GetIO();
    return io.Fonts->Fonts.Size > 1 ? io.Fonts->Fonts[1] : io.Fonts->Fonts[0];
}

// The UI test opens every section so that all the controls get drawn.
bool g_openAllGroups = false;

// A section of a tab: a bar with an icon and a name that opens and closes.
bool Group(const char* label)
{
    static const std::pair<const char*, const char*> icons[] = {
        { "Time", ICON_FA_CLOCK },
        { "HUD and screen", ICON_FA_EYE },
        { "Replay display", ICON_FA_LIST },
        { "Rage art and heat effects", ICON_FA_BOLT },
        { "Filmtweaks", ICON_FA_IMAGE },
        { "Depth of field", ICON_FA_CAMERA },
        { "Lens", ICON_FA_CIRCLE },
        { "Fog", ICON_FA_SMOG },
        { "Sun and sky", ICON_FA_SUN },
        { "Lighting", ICON_FA_LIGHTBULB },
        { "Quality", ICON_FA_SLIDERS },
        { "Stage", ICON_FA_CUBE },
        { "Frame", ICON_FA_EXPAND },
        { "ReShade", ICON_FA_WAND_MAGIC_SPARKLES },
        { "Engine", ICON_FA_GEAR },
        { "Your own", ICON_FA_SLIDERS },
        { "Console", ICON_FA_TERMINAL },
        { "Actors", ICON_FA_PERSON },
        { "Lights", ICON_FA_LIGHTBULB },
        { "Props", ICON_FA_CUBE },
        { "Effects", ICON_FA_WAND_MAGIC_SPARKLES },
        { "Free camera controls", ICON_FA_KEYBOARD },
        { "Fine tuning", ICON_FA_SLIDERS },
    };
    const char* icon = ICON_FA_SLIDERS;
    for (const auto& [name, glyph] : icons)
        if (!strcmp(name, label))
            icon = glyph;
    if (g_openAllGroups)
        ImGui::SetNextItemOpen(true);
    ImGui::Dummy({ 0, 1 });
    return ImGui::CollapsingHeader((std::string(icon) + "  " + label + "###" + label).c_str());
}

// Where things ended up on screen, for the UI test to click on.
struct Layout {
    float Left = 0, Width = 1, RulerTop = 0, CameraTop = 0, LookTop = 0, SpeedTop = 0, RangeTop = 0, Bottom = 0;
    ImVec2 PlayButton, OneX, SliderMin, SliderMax, ViewCenter, PanelCenter;
    ImVec2 Tabs[TabCount];
} g_layout;

void Help(const char* text)
{
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 24);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

// A bar with a name on it, over a part of a tab that's always open.
void Section(const char* title)
{
    ImGui::Dummy({ 0, 1 });
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const float height = ImGui::GetFrameHeight();
    ImGui::GetWindowDrawList()->AddRectFilled(at, { at.x + ImGui::GetContentRegionAvail().x, at.y + height }, ImGui::GetColorU32(ImGuiCol_Header));
    ImGui::SetCursorScreenPos({ at.x + ImGui::GetStyle().FramePadding.x * 2, at.y });
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(title);
}

// A row of a tab: the name on the left, in a column two fifths of the panel wide, and the control after it filling the rest. `keep` is
// the room left free at the end of the row, for the (?) or a button.
void Row(const char* label, float keep = -1)
{
    const ImGuiStyle& style = ImGui::GetStyle();
    const float right = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x; // the panel's right edge, the same for every row
    const float column = std::floor(right * 0.4f);
    if (keep < 0)
        keep = ImGui::GetFontSize() * 1.7f;
    ImGui::AlignTextToFramePadding();
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const float room = column - ImGui::GetCursorPosX() - style.ItemInnerSpacing.x;
    const char* hidden = strstr(label, "##"); // what follows ## is the control's id, not its name
    const char* end = hidden ? hidden : label + strlen(label);
    ImGui::PushClipRect(at, { at.x + std::max(room, 1.0f), at.y + ImGui::GetFrameHeight() }, true);
    ImGui::TextUnformatted(label, end);
    ImGui::PopClipRect();
    // A name too long for its column is cut off, and shown whole under the pointer.
    if (ImGui::IsItemHovered() && ImGui::CalcTextSize(label, end).x > room)
        ImGui::SetTooltip("%.*s", static_cast<int>(end - label), label);
    ImGui::SameLine(column);
    ImGui::SetNextItemWidth(std::max(right - column - keep, ImGui::GetFontSize() * 2));
}

// Room at the end of a row for a small button after the control.
float KeepButton()
{
    return ImGui::GetFontSize() * 3.8f;
}

// Kept for the few controls that sit beside something else on their line.
void BarWidth()
{
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.56f);
}

bool SliderDouble(const char* label, double& value, double min, double max, const char* format = "%.2f", ImGuiSliderFlags flags = 0)
{
    float f = static_cast<float>(value);
    Row(label);
    const bool changed = ImGui::SliderFloat((std::string("##") + label).c_str(), &f, static_cast<float>(min), static_cast<float>(max), format, flags);
    if (changed)
        value = f;
    return changed;
}

// A moment on the clock as minutes and seconds, the way the timeline writes it.
std::string TimeText(double seconds, int decimals = 2)
{
    const bool negative = seconds < 0;
    seconds = std::fabs(seconds);
    const int minutes = static_cast<int>(seconds / 60.0);
    char text[32];
    snprintf(text, sizeof(text), "%s%d:%0*.*f", negative ? "-" : "", minutes, decimals > 0 ? decimals + 3 : 2, decimals, seconds - minutes * 60.0);
    return text;
}

// ----- REPLAYS tab

std::string DateText(uint64_t seconds)
{
    const std::time_t time = static_cast<std::time_t>(seconds);
    std::tm tm {};
    localtime_s(&tm, &time);
    char text[32];
    std::strftime(text, sizeof(text), "%Y-%m-%d %H:%M", &tm);
    return text;
}

// 30202 is game version 3.02.02.
std::string VersionText(uint32_t version)
{
    char text[32];
    snprintf(text, sizeof(text), "%u.%02u.%02u", version / 10000, (version / 100) % 100, version % 100);
    return text;
}

// The editor's text only runs left to right, so a name in Arabic letters is turned around to read correctly.
std::string RightToLeftFixed(const std::string& text)
{
    std::vector<std::string> characters;
    bool arabic = false;
    for (size_t i = 0; i < text.size();) {
        const unsigned char first = text[i];
        const size_t length = first < 0x80 ? 1 : first < 0xE0 ? 2 : first < 0xF0 ? 3 : 4;
        characters.push_back(text.substr(i, length));
        arabic = arabic || first == 0xD8 || first == 0xD9 || first == 0xDA || first == 0xDB;
        i += length;
    }
    if (!arabic)
        return text;
    std::string out;
    for (auto it = characters.rbegin(); it != characters.rend(); ++it)
        out += *it;
    return out;
}

void ReplaysTab()
{
    static std::vector<sharing::Slot> slots;
    static bool loaded = false;
    static int selected = -1;
    static char search[64] = "";
    static char exportName[96] = "";
    static std::vector<std::string> files;
    static std::string message;

    const auto refresh = [] {
        slots = sharing::Slots();
        std::sort(slots.begin(), slots.end(), [](const sharing::Slot& a, const sharing::Slot& b) { return a.Played > b.Played; });
        files = sharing::SharedFiles();
        std::string names;
        for (sharing::Slot& slot : slots) {
            slot.Player1 = RightToLeftFixed(slot.Player1);
            slot.Player2 = RightToLeftFixed(slot.Player2);
            names += slot.Player1 + slot.Player2;
        }
        overlay::AddFontCharacters(names);
    };
    if (!loaded) {
        loaded = true;
        refresh();
    }

    ImGui::Text("%d replays", static_cast<int>(slots.size()));
    Help("Play a replay from the game's own Replay menu, under My Replays & Tips or Downloaded Replays. In its menu (B), set the three tips to Hide, or the replay stops at every tip.");
    ImGui::SameLine(ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize("Refresh").x - ImGui::GetStyle().FramePadding.x * 2);
    if (ImGui::SmallButton("Refresh"))
        refresh();
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##search", "Search...", search, sizeof(search));

    if (ImGui::BeginChild("replaylist", ImVec2(0, ImGui::GetContentRegionAvail().y * 0.36f), ImGuiChildFlags_Borders)) {
        for (const sharing::Slot& slot : slots) {
            const std::string when = DateText(slot.Played);
            const std::string row = when + "  " + slot.Player1 + " vs " + slot.Player2;
            if (search[0] && row.find(search) == std::string::npos)
                continue;
            ImGui::PushID(slot.Index);
            if (ImGui::Selectable(row.c_str(), selected == slot.Index, ImGuiSelectableFlags_AllowDoubleClick)) {
                selected = slot.Index;
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && !switcher::Start(slot.Index, message))
                    mod::Log("Switch: %s", message.c_str());
            }
            ImGui::PopID();
        }
    }
    ImGui::EndChild();

    if (switcher::Running()) {
        ImGui::TextUnformatted(switcher::Status().c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Cancel"))
            switcher::Cancel();
    } else {
        ImGui::BeginDisabled(selected < 0);
        if (ImGui::Button("Play this replay") && !switcher::Start(selected, message))
            mod::Log("Switch: %s", message.c_str());
        ImGui::EndDisabled();
        if (!switcher::Status().empty()) {
            ImGui::SameLine();
            ImGui::TextColored(Dim, "%s", switcher::Status().c_str());
        }
        Help("Leaves this replay and starts the one you picked, through the game's own screens: it makes the calls they make when clicked, so it works with the game in the background and however the list is sorted. Double-click a row does the same.");
    }

    Section("Mods");
    {
        const std::vector<pakmods::Mod> mods = pakmods::List();
        if (mods.empty())
            ImGui::TextColored(Dim, "No pak mods in the game's Paks folders.");
        const bool ready = pakmods::Available();
        ImGui::BeginDisabled(!ready);
        for (const pakmods::Mod& mod : mods) {
            bool on = mod.Mounted;
            ImGui::PushID(mod.Name.c_str());
            if (ImGui::Checkbox(mod.Name.c_str(), &on))
                pakmods::SetMounted(mod.Name, on);
            if (mod.Busy) {
                ImGui::SameLine();
                ImGui::TextColored(Dim, "...");
            }
            ImGui::PopID();
        }
        ImGui::EndDisabled();
        if (!ready && !mods.empty())
            ImGui::TextColored(Dim, "%s", pakmods::LastMessage().empty() ? "Finding the game's pak manager..." : pakmods::LastMessage().c_str());
        else if (!pakmods::LastMessage().empty())
            ImGui::TextWrapped("%s", pakmods::LastMessage().c_str());
        if (ImGui::SmallButton("Look again##paks"))
            pakmods::Rescan();
        Help("Switches your own pak mods off and on while the game runs. Something already on screen, like a fighter's outfit or the HUD, changes when the game loads it again: play this replay again or switch to another one. HUD pictures stay in memory until the game restarts. A mod you switch off stays off the next time the game starts, until you switch it on here.");
    }

    Section("Share a replay");
    // Export takes the replay that's playing, and the one picked in the list when none is.
    const int playing = replay::CanJump() ? gameui::OpenSlot() : -1;
    const int toExport = playing >= 0 ? playing : selected;
    if (playing >= 0) {
        const auto open = std::find_if(slots.begin(), slots.end(), [&](const sharing::Slot& s) { return s.Index == playing; });
        if (open != slots.end())
            ImGui::TextColored(Dim, "Playing: %s  %s vs %s", DateText(open->Played).c_str(), open->Player1.c_str(), open->Player2.c_str());
    } else if (replay::CanJump()) {
        ImGui::TextColored(Dim, "The replay that's playing isn't one of your saved replays, or it was opened before the mod saw it.");
    }
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.6f);
    ImGui::InputTextWithHint("##exportname", "Name (empty: date and players)", exportName, sizeof(exportName));
    ImGui::SameLine();
    ImGui::BeginDisabled(toExport < 0);
    if (ImGui::Button(playing >= 0 ? "Export this replay" : "Export")) {
        std::string error, name = exportName;
        const bool saved = name.empty() ? sharing::ExportNamed(toExport, name, error) : sharing::Export(toExport, name, error);
        message = saved ? "Saved " + name + ".t8replay" : error;
        files = sharing::SharedFiles();
    }
    ImGui::EndDisabled();
    Help("Saves the replay you're watching as a file, or the one picked in the list when no replay is playing, to send to a friend. It holds both players' names. With no name, the file is named after the date and the players.");
    ImGui::Checkbox("Export Replay in the game's own replay list", &gameui::ExportButtonOn());
    Help("Turns the greyed-out Show P2 Profile in the box a replay opens in the game's list into Export Replay. It saves that replay here, named after its date and players. The profiles can't be shown offline anyway.");
    ImGui::Checkbox("Play replays from older versions", &sharing::OlderReplaysOn());
    Help("The game won't play a replay saved before its last update. With this on, older replays (and older .t8replay files) are marked with the current version, so they play from the game's own list. If the list was open, leave it and come back. A replay is the players' button presses played through this version's moves, so if a patch changed a move they used, the fight can play out differently from the real match.");
    if (ImGui::SmallButton("Open the folder"))
        ShellExecuteA(nullptr, "open", sharing::Folder().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    ImGui::TextColored(Dim, "Files here can be loaded:");
    for (const std::string& file : files) {
        ImGui::PushID(file.c_str());
        if (ImGui::SmallButton("Load")) {
            std::string error;
            if (sharing::Import(file, error)) {
                message = "Loaded. It's in the game's Replay > Downloaded Replays. If that screen is open, leave it and come back. It stays until you close the game.";
                refresh();
            } else {
                message = error;
            }
        }
        ImGui::SameLine();
        ImGui::TextUnformatted(file.c_str());
        ImGui::PopID();
    }
    if (!message.empty())
        ImGui::TextWrapped("%s", message.c_str());

    Section("Shots");
    ImGui::TextWrapped("Camera path, speed ramp and look together. Give a friend the same replay and this file for your exact shot.");
    static char shotName[64] = "shot1";
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.6f);
    ImGui::InputText("##shot", shotName, sizeof(shotName));
    ImGui::SameLine();
    if (ImGui::Button("Save##shot"))
        Notice(presets::SaveShot(shotName) ? "Shot saved" : "Couldn't save the shot");
    for (const std::string& saved : presets::SavedShots()) {
        if (ImGui::SmallButton((saved + "##loadshot").c_str()))
            Notice(presets::LoadShot(saved) ? "Shot loaded" : "Couldn't load the shot");
        ImGui::SameLine();
    }
    ImGui::NewLine();
}

// ----- CAMERA tab

void AttachControls()
{
    camera::AttachSettings& a = camera::Attach();
    ImGui::RadioButton("P1", &a.Fighter, 0);
    ImGui::SameLine();
    ImGui::RadioButton("P2", &a.Fighter, 1);

    int style = static_cast<int>(a.Style);
    ImGui::SameLine();
    Row("Style");
    if (ImGui::Combo("##Style", &style, "Fixed to bone\0Orbit around bone\0Watch bone\0Fighter POV\0"))
        a.Style = static_cast<camera::AttachStyle>(style);

    if (a.Style == camera::AttachStyle::Pov) {
        ImGui::Checkbox("Hide their head", &a.PovHideHead);
        Help("Their own head and hair would fill the picture. Off shows them, for a mask or a helmet you want in view.");
        ImGui::SameLine();
        ImGui::Checkbox("Keep the horizon level", &a.PovLevel);
        Row("Eye position");
        ImGui::SliderFloat3("##Eye position", a.PovOffset, -40.0f, 40.0f, "%.0f");
        Help("Forward, right and up from the head bone, in centimetres. Fighters are different sizes, so nudge this if the view starts inside a face.");
        Row("Look");
        ImGui::SliderFloat3("##Look", a.PovTurn, -180.0f, 180.0f, "%.0f");
        Row("POV field of view");
        ImGui::SliderFloat("##POV field of view", &a.PovFov, 40.0f, 130.0f, "%.0f");
        Row("Smoothing");
        ImGui::SliderFloat("##Smoothing", &a.Smooth, 0.0f, 0.98f, "%.2f");
        return;
    }

    const std::vector<std::string> bones = camera::Bones(a.Fighter);
    Row("Bone");
    if (ImGui::BeginCombo("##Bone", a.Bone[0] ? a.Bone : "(whole fighter)", ImGuiComboFlags_HeightLarge)) {
        static char filter[32] = "";
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##bonefilter", "Search, e.g. head or hand", filter, sizeof(filter));
        if (ImGui::Selectable("(whole fighter)", !a.Bone[0]))
            a.Bone[0] = 0;
        for (const std::string& bone : bones) {
            if (filter[0] && bone.find(filter) == std::string::npos)
                continue;
            if (ImGui::Selectable(bone.c_str(), bone == a.Bone))
                strncpy_s(a.Bone, bone.c_str(), _TRUNCATE);
        }
        ImGui::EndCombo();
    }
    if (bones.empty())
        ImGui::TextColored(Dim, "Bones appear once a fight is on.");

    if (a.Style == camera::AttachStyle::Fixed) {
        Row("Offset");
        ImGui::SliderFloat3("##Offset", a.Offset, -300.0f, 300.0f, "%.0f");
        Row("Turn");
        ImGui::SliderFloat3("##Turn", a.Turn, -180.0f, 180.0f, "%.0f");
    } else if (a.Style == camera::AttachStyle::Orbit) {
        Row("Distance");
        ImGui::SliderFloat("##Distance", &a.OrbitDistance, 10.0f, 3000.0f, "%.0f cm", ImGuiSliderFlags_Logarithmic);
        ImGui::TextColored(Dim, "Right mouse turns, wheel sets the distance.");
    }
    Row("Smoothing");
    ImGui::SliderFloat("##Smoothing", &a.Smooth, 0.0f, 0.98f, "%.2f");
}

void CameraTab()
{
    camera::Settings& s = camera::Config();

    Section("Camera");
    const int mode = static_cast<int>(s.Mode);
    if (ImGui::RadioButton("Game", mode == 0))
        s.Mode = camera::Mode::Game;
    ImGui::SameLine();
    if (ImGui::RadioButton("Free (F5)", mode == 1) && s.Mode != camera::Mode::Free)
        camera::ToggleFree();
    if (ImGui::RadioButton("Dolly (F6)", mode == 2) && s.Mode != camera::Mode::Dolly)
        camera::ToggleDolly();
    ImGui::SameLine();
    if (ImGui::RadioButton("Attach to fighter", mode == 3))
        s.Mode = camera::Mode::Attach;

    SliderDouble("Field of view", s.Fov, 5.0, 130.0, "%.1f");
    if (s.Mode == camera::Mode::Game)
        ImGui::Checkbox("Use this with the game camera", &s.FovOverride);

    if (s.Mode == camera::Mode::Attach) {
        Section("Attach");
        AttachControls();
    }
    if (s.Mode == camera::Mode::Free || s.Mode == camera::Mode::Attach) {
        SliderDouble("Roll", s.Roll, -180.0, 180.0, "%.1f");
    }

    if (Group("Free camera controls")) {
        ImGui::TextColored(Dim, "W A S D fly, Q E down and up");
        ImGui::TextColored(Dim, "Right mouse over the picture to look");
        ImGui::TextColored(Dim, s.LookInput == camera::LookInput::Mouse ? "F4: looking with the mouse. Shift faster, Ctrl slower. Wheel rolls, Ctrl+wheel zooms, Shift+wheel sets speed"
                                                         : "F4: looking with the arrow keys. Shift faster, Ctrl slower. Wheel rolls, Ctrl+wheel zooms, Shift+wheel sets speed");
        ImGui::TextColored(Dim, "Z C roll, R level, + - zoom, K add a key");
        SliderDouble("Fly speed", s.FlySpeed, 10.0, 5000.0, "%.0f cm/s", ImGuiSliderFlags_Logarithmic);
        SliderDouble("Look speed", s.LookSpeed, 0.02, 0.5, "%.2f");
    }

    Section("Rage art and heat smash camera");
    const int filter = static_cast<int>(s.Filter);
    if (ImGui::RadioButton("Game default", filter == 0))
        s.Filter = camera::Filter::Off;
    if (ImGui::RadioButton("Keep the fight camera", filter == 1))
        s.Filter = camera::Filter::Cinematics;
    Help("When the game cuts to a cinematic angle, the camera stays on the normal fight framing until it's over.");
    if (ImGui::RadioButton("Classic 2D, always", filter == 2))
        s.Filter = camera::Filter::Classic;
    Help("A fixed side-on camera for the whole fight. The combo camera moves are gone too.");
    if (camera::InCinematicHold())
        ImGui::TextColored(Good, "Holding the fight camera now");
    if (s.Filter != camera::Filter::Off && Group("Fine tuning")) {
        SliderDouble("Cut distance", s.JumpDistance, 10.0, 300.0, "%.0f cm");
        SliderDouble("Cut angle", s.JumpAngle, 2.0, 45.0, "%.0f deg");
        SliderDouble("Back off as they part", s.SeparationZoom, 0.0, 1.5, "%.2f");
    }

    Section("Dolly path");
    ImGui::Checkbox("Run on real time", &s.PathOnRealTime);
    Help("Off: the path follows the fight clock, so it slows and stops with the game. On: it plays in real seconds, for moves around a paused moment.");
    if (s.PathOnRealTime) {
        ImGui::SameLine();
        if (ImGui::SmallButton("Restart"))
            camera::RestartPathClock();
    }
    int pathStyle = static_cast<int>(s.PathStyle);
    Row("Dolly style");
    if (ImGui::Combo("##Dolly style", &pathStyle, "Smooth\0Linear\0CoDMVM\0"))
        s.PathStyle = static_cast<cine::PathStyle>(pathStyle);
    Help("Smooth: a curve through every key that keeps a steady speed however the keys are spaced. Linear: straight lines at constant speed, with a sharp corner at each key. CoDMVM: a plain curve through every key, where each stretch takes its own speed from how far apart its keys are.");

    static char name[64] = "path1";
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.4f);
    ImGui::InputText("##pathname", name, sizeof(name));
    ImGui::SameLine();
    if (ImGui::Button("Save##path"))
        Notice(camera::SavePath(name) ? "Path saved" : "Couldn't save the path");
    ImGui::SameLine();
    if (ImGui::Button("CamIO"))
        Notice(camera::ExportCamIo(name, recorder::Config().Fps) ? "Wrote an HLAE .cam file" : "A path needs at least 2 keys");
    Help("Writes an HLAE .cam file of the path, for matching the camera in After Effects or Blender.");
    for (const std::string& saved : camera::SavedPaths()) {
        if (ImGui::SmallButton((saved + "##loadpath").c_str()))
            camera::LoadPath(saved);
        ImGui::SameLine();
    }
    ImGui::NewLine();

    const camera::View view = camera::LastView();
    if (view.Valid) {
        Section("Position");
        ImGui::TextColored(Dim, "X %.0f  Y %.0f  Z %.0f", view.Location.X, view.Location.Y, view.Location.Z);
        ImGui::TextColored(Dim, "Pitch %.1f  Yaw %.1f  Roll %.1f  FOV %.1f", view.Rotation.Pitch, view.Rotation.Yaw, view.Rotation.Roll, view.Fov);
    }
}

// ----- Look tab

void TimeGroup()
{
    game::TimeControl& t = game::Time();
    float speed = static_cast<float>(t.Speed);
    Row("Timescale");
    ImGui::BeginDisabled(t.UseSpeedRamp);
    if (ImGui::SliderFloat("##Timescale", &speed, static_cast<float>(MinSpeed), static_cast<float>(MaxSpeed), "%.3f", ImGuiSliderFlags_Logarithmic))
        t.Speed = speed;
    ImGui::EndDisabled();
    ImGui::Checkbox("Speed ramp from the timeline", &t.UseSpeedRamp);
    ImGui::Checkbox("Blend motion between game steps", &t.Interpolate);
    Help("The game only moves in 60 Hz steps. Blending poses between steps is what makes slow motion smooth.");
    ImGui::Checkbox("No slow motion on big hits and KOs", &t.NoGameSlowMotion);
    Help("The game almost stops the fight for a moment on some hits and at a KO. This runs straight through those moments. The replay clock skips ahead with them, so a camera path jumps by that much there.");
    ImGui::Checkbox("Slow the effects too", &t.ScaleEffects);
    Help("Sparks, cloth and materials follow the timescale through the engine's time dilation.");
    float cap = static_cast<float>(t.FpsCap);
    Row("FPS cap");
    if (ImGui::SliderFloat("##FPS cap", &cap, 0.0f, 360.0f, cap <= 0 ? "game default" : "%.0f"))
        t.FpsCap = cap;
}

void DvarGroup(const char* group)
{
    for (dvars::Slider& slider : dvars::All()) {
        if (slider.Group != group)
            continue;
        ImGui::PushID(slider.Label.c_str());
        Row(slider.Label.c_str(), ImGui::GetFontSize() * 2.2f); // room for the x that gives it back to the game
        ImGuiSliderFlags flags = slider.Logarithmic ? ImGuiSliderFlags_Logarithmic : 0;
        if (!slider.Choices.empty()) {
            int choice = static_cast<int>(slider.Value);
            const char* shown = slider.Overridden && choice >= 0 && choice < static_cast<int>(slider.Choices.size()) ? slider.Choices[choice].c_str() : "game";
            if (ImGui::BeginCombo("##choice", shown)) {
                for (int i = 0; i < static_cast<int>(slider.Choices.size()); ++i)
                    if (ImGui::Selectable(slider.Choices[i].c_str(), slider.Overridden && i == choice)) {
                        slider.Value = static_cast<float>(i);
                        dvars::Changed(slider);
                    }
                ImGui::EndCombo();
            }
            if (slider.Overridden) {
                ImGui::SameLine();
                if (ImGui::SmallButton("x"))
                    dvars::Reset(slider);
            }
            ImGui::PopID();
            continue;
        }
        const char* format = slider.Whole ? "%.0f" : (slider.Max <= 2 ? "%.3f" : "%.1f");
        if (ImGui::SliderFloat("##value", &slider.Value, slider.Min, slider.Max, slider.Overridden ? format : "game", flags)) {
            if (slider.Whole)
                slider.Value = std::round(slider.Value);
            dvars::Changed(slider);
        }
        if (slider.Overridden) {
            ImGui::SameLine();
            if (ImGui::SmallButton("x"))
                dvars::Reset(slider);
        }
        ImGui::PopID();
    }
}

void PostFxGroup(const char* group)
{
    for (postfx::Control& control : postfx::All()) {
        if (std::string(control.Group) != group)
            continue;
        ImGui::PushID(control.Property);
        ImGui::Checkbox("##on", &control.On);
        ImGui::SameLine();
        Row(control.Label);
        const char* format = control.Max >= 100 ? "%.0f" : "%.2f";
        if (ImGui::SliderFloat("##value", &control.Value, control.Min, control.Max, format)) {
            control.On = true;
            // Autofocus writes this one every frame, so moving it by hand turns autofocus off.
            if (std::string(control.Property) == "DepthOfFieldFocalDistance")
                postfx::FocusConfig().Mode = postfx::Focus::Manual;
        }
        ImGui::PopID();
    }
}

void ReShadeGroup();

void VisualsTab()
{
    effects::Settings& e = effects::Config();
    bool changed = false;

    static char presetName[64] = "look1";
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.5f);
    ImGui::InputText("##preset", presetName, sizeof(presetName));
    ImGui::SameLine();
    if (ImGui::Button("Save preset"))
        Notice(presets::SavePreset(presetName) ? "Preset saved" : "Couldn't save the preset");
    for (const std::string& saved : presets::SavedPresets()) {
        if (ImGui::SmallButton((saved + "##loadpreset").c_str()))
            Notice(presets::LoadPreset(saved) ? "Preset loaded" : "Couldn't load the preset");
        ImGui::SameLine();
    }
    ImGui::NewLine();

    ImGui::SetNextItemOpen(true, ImGuiCond_FirstUseEver);
    if (Group("Time"))
        TimeGroup();

    ImGui::SetNextItemOpen(true, ImGuiCond_FirstUseEver);
    if (Group("HUD and screen")) {
        Row("HUD preset");
        if (ImGui::BeginCombo("##HUD preset", effects::HudPresetName(effects::HudPreset()))) {
            for (int preset = 0; preset < effects::HudPresetCount; ++preset)
                if (ImGui::Selectable(effects::HudPresetName(preset), preset == effects::HudPreset()))
                    effects::ApplyHudPreset(preset);
            ImGui::EndCombo();
        }
        Help("Fragmovie leaves the health bars, the timer and the hit combo and damage text. Broadcast leaves the match HUD (bars, timer, names, round wins) and takes away the replay's panels. Ticking anything below makes it Custom.");
        if (ImGui::Checkbox("Hide the HUD (F9)", &e.HideHud)) {
            e.KeepHud[0] = 0;
            effects::HudPreset() = -1;
        }
        if (ImGui::Checkbox("Keep the in-battle status", &e.KeepBattleStatus))
            effects::HudPreset() = -1;
        Help("With the HUD hidden, this leaves the hit combo, damage, counter, clean, punish and tornado text on screen. Choose which of them under Replay display.");
        changed |= ImGui::Checkbox("No bloom", &e.NoBloom);
        changed |= ImGui::Checkbox("No motion blur", &e.NoMotionBlur);
        changed |= ImGui::Checkbox("No depth of field", &e.NoDepthOfField);
        changed |= ImGui::Checkbox("No particles", &e.NoParticles);
        changed |= ImGui::Checkbox("No vignette", &e.NoVignette);
    }

    if (Group("Replay display")) {
        for (int row = 0; row < replaydisplay::Rows; ++row) {
            int shown = replaydisplay::Choice(row) + 1;
            ImGui::PushID(row);
            Row(replaydisplay::Label(row));
            const bool picked = ImGui::Combo("##shown", &shown, "Leave to the game\0Hide\0Show\0");
            ImGui::PopID();
            if (picked) {
                replaydisplay::Choice(row) = shown - 1;
                replaydisplay::Changed();
            }
        }
        Help("Changes straight away and holds through the rounds. Leave to the game gives the row back to the game's own setting from the next replay you open.");
        ImGui::TextColored(Dim, "In-battle status");
        for (int item = 0; item < replaydisplay::StatusItems; ++item)
            if (ImGui::Checkbox(replaydisplay::StatusLabel(item), &replaydisplay::StatusShown(item)))
                replaydisplay::Changed();
    }

    if (Group("Rage art and heat effects")) {
        changed |= ImGui::Checkbox("Hide hit sparks and glows", &e.NoHitEffects);
        Help("Hides the sparks of hits and the glows of rage arts and heat as they appear. Untick to get them back.");
        changed |= ImGui::Checkbox("No rage art screen effects", &e.NoDramaManager);
        Help("Cancels what a rage art or heat smash puts over the picture while it plays.");
    }
    if (changed)
        effects::RequestRefresh();

    if (Group("Filmtweaks"))
        PostFxGroup("Filmtweaks");

    if (Group("Depth of field")) {
        ImGui::TextColored(Dim, "Needs Depth of field quality above 0 under Engine.");
        int focus = static_cast<int>(postfx::FocusConfig().Mode);
        Row("Autofocus");
        if (ImGui::Combo("##Autofocus", &focus, "Off\0On P1\0On P2\0Between them\0From the camera\0"))
            postfx::FocusConfig().Mode = static_cast<postfx::Focus>(focus);
        PostFxGroup("Depth of field");
    }

    if (Group("Lens"))
        PostFxGroup("Lens");

    if (Group("Fog")) {
        DvarGroup("Fog");
        dvars::Colors& tint = dvars::Tint();
        Row("Fog colour");
        if (ImGui::ColorEdit3("##Fog colour", tint.Fog, ImGuiColorEditFlags_NoInputs)) {
            tint.FogOverridden = true;
            dvars::TintChanged();
        }
        if (tint.FogOverridden) {
            ImGui::SameLine();
            if (ImGui::SmallButton("Map's own##fog")) {
                tint.FogOverridden = false;
                dvars::TintChanged();
            }
        }
        ImGui::Checkbox("Fog starts at the camera", &tint.FogFromCamera);
        Help("Moves the fog with the camera and takes its height falloff away, so it thickens only with distance from you. Fly up or down and it stays the same.");
        ImGui::Checkbox("Hide the map's mist and drifting particles with it", &effects::Config().FogHidesMist);
        Help("The map's mist is made of particles, which fog settings don't reach. This hides them while a fog colour is on. Hit sparks stay.");
    }

    if (Group("Sun and sky")) {
        DvarGroup("Light");
        dvars::Colors& tint = dvars::Tint();
        Row("Sun colour");
        if (ImGui::ColorEdit3("##Sun colour", tint.Sun, ImGuiColorEditFlags_NoInputs)) {
            tint.SunOverridden = true;
            dvars::TintChanged();
        }
        sky::Settings& k = sky::Config();
        Row("Sun direction");
        ImGui::SliderFloat("##Sun direction", &k.SunYaw, -180.0f, 180.0f, "%.0f deg");
        Row("Sun height");
        ImGui::SliderFloat("##Sun height", &k.SunHeight, -60.0f, 60.0f, "%.0f deg");
        Help("Turns and tilts the stage's sun from where the stage put it, so the light and the shadows move. 0 is the stage's own.");
        Row("Sky rotation");
        ImGui::SliderFloat("##Sky rotation", &k.SkyRotation, -180.0f, 180.0f, "%.0f deg");
        ImGui::Checkbox("Sky picture of your own (HDRI)", &k.HdriOn);
        Help("Puts a panorama of yours on the stage's sky. .hdr and .exr are real HDRIs, and .png and .jpg work too. It should be the usual HDRI shape, twice as wide as it is high.");
        if (k.HdriOn) {
            static std::vector<std::string> files;
            static double listedAt = -10;
            if (ImGui::GetTime() - listedAt > 1.5) {
                files = sky::Files();
                listedAt = ImGui::GetTime();
            }
            Row("Picture");
            if (ImGui::BeginCombo("##Picture", k.HdriFile[0] ? k.HdriFile : "Choose one")) {
                for (const std::string& file : files)
                    if (ImGui::Selectable(file.c_str(), file == k.HdriFile))
                        strncpy_s(k.HdriFile, file.c_str(), _TRUNCATE);
                if (files.empty())
                    ImGui::TextColored(Dim, "No pictures in the folder yet");
                ImGui::EndCombo();
            }
            if (ImGui::SmallButton("Open the pictures folder"))
                ShellExecuteW(nullptr, L"open", sky::Folder().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            Row("Picture brightness");
            ImGui::SliderFloat("##Picture brightness", &k.HdriBrightness, 0.02f, 20.0f, "%.2fx", ImGuiSliderFlags_Logarithmic);
            ImGui::Checkbox("Light the stage from it", &k.HdriLightsStage);
            Help("The stage's sky light takes its light from your picture and not from the stage's own sky. The sun stays where it is: turn it with the sliders above to match your picture.");
            const std::string status = sky::Status();
            if (!status.empty())
                ImGui::TextColored(Dim, "%s", status.c_str());
        }
    }

    if (Group("Lighting")) {
        scene::Settings& s = scene::Config();
        const scene::Counts counts = scene::LastCounts();
        ImGui::Checkbox("Character lights off", &s.NoCharacterLights);
        Help("Takes the stage's key light off the fighters, and their own spot lights. They're still lit by the sky and the stage around them.");
        ImGui::Checkbox("Stage lights off", &s.NoStageLights);
        Help("Every other light, the sun included. Your lighting rig's lights stay on.");
        ImGui::Checkbox("Colour the light on the fighters", &s.TintCharacterLights);
        ImGui::SameLine();
        ImGui::ColorEdit3("##charlight", s.CharacterLightColor, ImGuiColorEditFlags_NoInputs);
        if (s.NoCharacterLights || s.NoStageLights || s.TintCharacterLights)
            ImGui::TextColored(Dim, "%d character lights, %d stage lights on this stage", counts.CharacterLights, counts.StageLights);
    }

    if (Group("Quality")) {
        static int graphicsPreset = -1;
        Row("Graphics preset");
        if (ImGui::BeginCombo("##Graphics preset", dvars::GraphicsPresetName(graphicsPreset))) {
            for (int preset = -1; preset < dvars::GraphicsPresetCount; ++preset)
                if (ImGui::Selectable(dvars::GraphicsPresetName(preset), preset == graphicsPreset)) {
                    graphicsPreset = preset;
                    dvars::ApplyGraphicsPreset(preset);
                }
            ImGui::EndCombo();
        }
        Help("Sets every setting below, and the bloom, motion blur, depth of field and resolution scale under Engine, in one go. Extreme goes past the game's own maximum and renders at 150% resolution, so it's slow: use it for recording. Change any one setting after it as you like.");
        DvarGroup("Quality");
        Help("How the engine lights and smooths the picture, as TekkenOverlay's display settings change it. x puts the game's own back.");
    }

    if (Group("Stage")) {
        scene::Settings& s = scene::Config();
        ImGui::Checkbox("Plain stage", &s.PlainStage);
        Help("Every stage mesh in the engine's plain grey material, like TekkenOverlay's simplified stages. The fighters and effects keep theirs. Untick to get the stage back.");
        if (s.PlainStage)
            ImGui::TextColored(Dim, "%d stage meshes plain", scene::LastCounts().PlainMeshes);
        ImGui::Checkbox("No camera shake", &s.NoCameraShake);
        ImGui::Checkbox("Hide P1", &sky::Config().HideFighter[0]);
        ImGui::SameLine();
        ImGui::Checkbox("Hide P2", &sky::Config().HideFighter[1]);
        Help("Takes a fighter out of the picture, shadow included. Their hit sparks still show.");
        changed = ImGui::Checkbox("No particles (stage and fight)", &e.NoParticles) || changed;
        if (changed)
            effects::RequestRefresh();
    }

    if (Group("Frame")) {
        scene::Settings& s = scene::Config();
        int frame = static_cast<int>(s.Frame);
        Row("Shape");
        if (ImGui::Combo("##Shape", &frame, "The game's own\0Fill the screen (no black bars)\0Fixed aspect ratio\0"))
            s.Frame = static_cast<scene::Frame>(frame);
        if (s.Frame == scene::Frame::Fixed) {
            Row("Aspect ratio");
            ImGui::SliderFloat("##Aspect ratio", &s.AspectRatio, 1.0f, 3.0f, "%.2f : 1");
            for (const float preset : { 1.78f, 2.0f, 2.39f, 2.76f }) {
                char label[16];
                snprintf(label, sizeof(label), "%.2f##ar", preset);
                if (ImGui::SmallButton(label))
                    s.AspectRatio = preset;
                ImGui::SameLine();
            }
            ImGui::NewLine();
        }
        Help("Fill the screen takes away the bars the game adds on screens that aren't 16:9. A fixed aspect ratio adds bars of its own, such as 2.39 for a cinema look, and they're in the recording too.");
    }

    if (Group("ReShade"))
        ReShadeGroup();

    if (Group("Engine"))
        DvarGroup("Engine");

    bool anyOwn = false;
    for (const dvars::Slider& slider : dvars::All())
        anyOwn = anyOwn || slider.Group == "Your own";
    if (anyOwn && Group("Your own"))
        DvarGroup("Your own");

    if (Group("Console")) {
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.74f);
        const bool entered = ImGui::InputText("##command", e.CustomCommand, sizeof(e.CustomCommand), ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        if ((ImGui::Button("Run") || entered) && e.CustomCommand[0])
            effects::RunCommand(e.CustomCommand);
        ImGui::TextColored(Dim, "Add sliders of your own in tekken8mvm\\dvars.ini.");
    }
}

// ----- SCENE tab: what you put on the stage yourself

// A place in the world, as three numbers to drag: the stage is thousands of centimetres across, too far for a slider.
bool DragPlace(const char* label, float* values, float speed = 1.0f, const char* format = "%.0f")
{
    Row(label);
    return ImGui::DragFloat3((std::string("##") + label).c_str(), values, speed, 0, 0, format);
}

std::string Lowered(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

// One of the game's loaded meshes or effects by name, with a search box, or a full path typed in for one the game has to load.
void AssetPicker(const char* label, char* asset, size_t size, const std::vector<std::string>& names, const char* hint)
{
    static char filter[48] = "";
    std::string shown = asset;
    if (const size_t cut = shown.find_last_of("/."); cut != std::string::npos)
        shown.erase(0, cut + 1);
    Row(label);
    if (ImGui::BeginCombo((std::string("##") + label).c_str(), asset[0] ? shown.c_str() : "Choose one", ImGuiComboFlags_HeightLarge)) {
        if (ImGui::IsWindowAppearing()) {
            spawner::AskForLists();
            filter[0] = 0;
        }
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##assetfilter", hint, filter, sizeof(filter));
        const std::string wanted = Lowered(filter);
        int listed = 0;
        for (const std::string& name : names) {
            if (!wanted.empty() && Lowered(name).find(wanted) == std::string::npos)
                continue;
            if (++listed > 300) {
                ImGui::TextColored(Dim, "There are more. Type to narrow it down.");
                break;
            }
            if (ImGui::Selectable(name.c_str(), name == asset))
                strncpy_s(asset, size, name.c_str(), _TRUNCATE);
        }
        if (names.empty())
            ImGui::TextColored(Dim, "The list fills in once a fight is on.");
        ImGui::EndCombo();
    }
    Row("or a path");
    ImGui::InputTextWithHint("##assetpath", "/Game/...", asset, size);
}

void LightsGroup()
{
    for (int i = 0; i < lights::MaxLights; ++i) {
        lights::Light& light = lights::Rig()[i];
        ImGui::PushID(1000 + i); // each Scene group gets its own ID range, so "Here" in Light 1 and Actor 1 don't collide
        char title[24];
        snprintf(title, sizeof(title), "Light %d", i + 1);
        ImGui::Checkbox(title, &light.On);
        ImGui::SameLine();
        if (ImGui::SmallButton("Here"))
            lights::PlaceAtCamera(i);
        if (i == 0)
            Help("Here puts the light where the camera is, pointing the way it looks, and leaves it there. Fly somewhere, press Here, fly away.");
        if (light.On) {
            ImGui::SameLine();
            ImGui::Checkbox("Spot", &light.Spot);
            int follow = static_cast<int>(light.Follow);
            Row("Follows");
            if (ImGui::Combo("##Follows", &follow, "Nothing\0P1\0P2\0Between them\0"))
                light.Follow = static_cast<lights::Follow>(follow);
            DragPlace(light.Follow == lights::Follow::World ? "Place" : "Offset", light.Offset);
            Row("Brightness");
            ImGui::SliderFloat("##Brightness", &light.Intensity, 100000.0f, 1000000000.0f, "%.0f", ImGuiSliderFlags_Logarithmic);
            if (i == 0)
                Help("The game's stages run very bright: their own lights are between 30 and 100 million.");
            Row("Reach");
            ImGui::SliderFloat("##Reach", &light.Radius, 50.0f, 5000.0f, "%.0f cm");
            Row("Colour");
            ImGui::ColorEdit3("##Colour", light.Color, ImGuiColorEditFlags_NoInputs);
            ImGui::Checkbox("Shadows", &light.Shadows);
            if (light.Spot) {
                int aim = static_cast<int>(light.Aim);
                Row("Points at");
                if (ImGui::Combo("##Points at", &aim, "Its own direction\0P1\0P2\0Between them\0"))
                    light.Aim = static_cast<lights::Aim>(aim);
                if (light.Aim == lights::Aim::Direction) {
                    Row("Direction");
                    ImGui::DragFloat2("##Direction", light.Direction, 0.5f, -360.0f, 360.0f, "%.0f deg");
                }
                Row("Cone");
                ImGui::SliderFloat("##Cone", &light.Cone, 2.0f, 80.0f, "%.0f deg");
            }
        }
        ImGui::PopID();
    }
}

void PropsGroup(const std::vector<std::string>& meshes)
{
    static bool open[spawner::MaxProps] = {};
    auto& props = spawner::Props();
    for (int i = 0; i < spawner::MaxProps; ++i) {
        spawner::Prop& prop = props[i];
        if (!prop.On && !prop.Asset[0] && !open[i] && !g_openAllGroups)
            continue;
        ImGui::PushID(2000 + i);
        char title[24];
        snprintf(title, sizeof(title), "Prop %d", i + 1);
        ImGui::Checkbox(title, &prop.On);
        ImGui::SameLine();
        if (ImGui::SmallButton("Here"))
            spawner::PlacePropAtCamera(i);
        ImGui::SameLine();
        if (ImGui::SmallButton("Remove")) {
            prop = {};
            open[i] = false;
        }
        AssetPicker("Mesh", prop.Asset, sizeof(prop.Asset), meshes, "Search, e.g. rock or tree");
        Row("Follows");
        ImGui::Combo("##Follows", &prop.Follow, "Nothing\0P1\0P2\0Between them\0");
        DragPlace(prop.Follow == 0 ? "Place" : "Offset", prop.Location);
        DragPlace("Turn", prop.Rotation, 0.5f, "%.1f");
        Row("Size", KeepButton());
        ImGui::DragFloat3("##Size", prop.Scale, 0.01f, 0.01f, 100.0f, "%.2f");
        ImGui::SameLine();
        if (ImGui::SmallButton("Even"))
            prop.Scale[1] = prop.Scale[2] = prop.Scale[0];
        ImGui::Checkbox("Shadows", &prop.Shadows);
        const std::string status = spawner::PropStatus(i);
        if (!status.empty())
            ImGui::TextColored(Warn, "%s", status.c_str());
        ImGui::PopID();
        ImGui::Separator();
    }
    if (ImGui::SmallButton("+ Add a prop"))
        for (int i = 0; i < spawner::MaxProps; ++i)
            if (!props[i].On && !props[i].Asset[0] && !open[i]) {
                open[i] = true;
                spawner::PlacePropAtCamera(i);
                break;
            }
    Help("A prop is any mesh the game has: the list is what this stage has loaded, and a full path loads one from anywhere in the game's files, pak mods included. It appears a little in front of the camera. Props can't be walked into, and they stay through rewinds.");
}

void EffectsGroup(const std::vector<std::string>& systems)
{
    static bool open[spawner::MaxEffects] = {};
    auto& effects = spawner::Effects();
    for (int i = 0; i < spawner::MaxEffects; ++i) {
        spawner::Effect& effect = effects[i];
        if (!effect.On && !effect.Asset[0] && !open[i] && !g_openAllGroups)
            continue;
        ImGui::PushID(3000 + i);
        char title[24];
        snprintf(title, sizeof(title), "Effect %d", i + 1);
        ImGui::Checkbox(title, &effect.On);
        ImGui::SameLine();
        if (ImGui::SmallButton("Here"))
            spawner::PlaceEffectAtCamera(i);
        ImGui::SameLine();
        if (ImGui::SmallButton("Remove")) {
            effect = {};
            open[i] = false;
        }
        AssetPicker("Effect", effect.Asset, sizeof(effect.Asset), systems, "Search, e.g. hit, aura or smoke");
        Row("Follows");
        ImGui::Combo("##Follows", &effect.Follow, "Nothing\0P1\0P2\0Between them\0");
        DragPlace(effect.Follow == 0 ? "Place" : "Offset", effect.Location);
        DragPlace("Turn", effect.Rotation, 0.5f, "%.1f");
        Row("Size");
        ImGui::SliderFloat("##Size", &effect.Scale, 0.05f, 20.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
        ImGui::Checkbox("Runs on the timeline", &effect.OnTimeline);
        if (i == 0 || g_openAllGroups)
            Help("On: the effect's age comes from the fight clock, so it's in the same state at the same moment on every pass, rewinds with the replay and stands still while paused. Off: it plays on the game's own time, and Restart starts it over.");
        if (effect.OnTimeline) {
            float start = static_cast<float>(effect.Start);
            Row("Starts at", KeepButton());
            if (ImGui::DragFloat("##Starts at", &start, 0.01f, 0, 0, "%.2f s"))
                effect.Start = start;
            ImGui::SameLine();
            if (ImGui::SmallButton("Now"))
                effect.Start = game::Clock();
            Row("Shows for");
            ImGui::SliderFloat("##Shows for", &effect.Length, 0.0f, 30.0f, effect.Length <= 0 ? "from there on" : "%.2f s");
            Row("Rate");
            ImGui::SliderFloat("##Rate", &effect.Rate, 0.05f, 4.0f, "%.2fx", ImGuiSliderFlags_Logarithmic);
        } else if (ImGui::SmallButton("Restart")) {
            spawner::RestartEffect(i);
        }
        const std::string status = spawner::EffectStatus(i);
        if (!status.empty())
            ImGui::TextColored(Warn, "%s", status.c_str());
        ImGui::PopID();
        ImGui::Separator();
    }
    if (ImGui::SmallButton("+ Add an effect"))
        for (int i = 0; i < spawner::MaxEffects; ++i)
            if (!effects[i].On && !effects[i].Asset[0] && !open[i]) {
                open[i] = true;
                spawner::PlaceEffectAtCamera(i);
                break;
            }
    Help("An effect is any particle system the game has loaded: hit sparks, auras, smoke, the stage's own. It starts at this moment of the replay, a little in front of the camera.");
}

void ActorsGroup()
{
    recorder::Settings& rec = recorder::Config();
    ImGui::TextWrapped("A take is one fighter's motion over the timeline's range. An actor is a copy of that fighter playing a take, anywhere on the stage.");

    static char takeName[64] = "take1";
    static int side = 0;
    static std::string error;
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.4f);
    ImGui::InputText("##takename", takeName, sizeof(takeName));
    ImGui::SameLine();
    ImGui::RadioButton("P1##take", &side, 0);
    ImGui::SameLine();
    ImGui::RadioButton("P2##take", &side, 1);
    ImGui::TextColored(Dim, "Range: %.2f to %.2f s (%.2f s)", rec.RangeIn, rec.RangeOut, rec.RangeOut - rec.RangeIn);
    if (actors::RecordingTake()) {
        if (ImGui::Button("Cancel the take"))
            actors::CancelTake();
    } else if (ImGui::Button("Record a take")) {
        error.clear();
        actors::StartTake(takeName, side, rec.RangeIn, rec.RangeOut, error);
    }
    Help("Drag the range on the timeline over the motion you want, then press this. It goes to the start of the range, plays it at normal speed and stops at the end. The take is saved, so it's there next time too.");
    const std::string takeStatus = error.empty() ? actors::TakeStatus() : error;
    if (!takeStatus.empty())
        ImGui::TextColored(error.empty() ? Dim : Warn, "%s", takeStatus.c_str());

    const std::vector<actors::TakeInfo> takes = actors::Takes();
    for (const actors::TakeInfo& take : takes) {
        ImGui::PushID(take.Name.c_str());
        if (ImGui::SmallButton("Delete"))
            actors::DeleteTake(take.Name);
        ImGui::SameLine();
        ImGui::Text("%s", take.Name.c_str());
        ImGui::SameLine();
        ImGui::TextColored(Dim, "%.2f s, P%d", take.Frames / 60.0, take.Side + 1);
        ImGui::PopID();
    }

    static bool open[actors::MaxActors] = {};
    auto& cast = actors::Cast();
    for (int i = 0; i < actors::MaxActors; ++i) {
        actors::Actor& actor = cast[i];
        if (!actor.On && !actor.Take[0] && !open[i] && !g_openAllGroups)
            continue;
        ImGui::Separator();
        ImGui::PushID(4000 + i);
        char title[24];
        snprintf(title, sizeof(title), "Actor %d", i + 1);
        ImGui::Checkbox(title, &actor.On);
        ImGui::SameLine();
        if (ImGui::SmallButton("Here"))
            actors::PlaceAtCamera(i);
        ImGui::SameLine();
        if (ImGui::SmallButton("Remove")) {
            actor = {};
            open[i] = false;
        }
        Row("Take");
        if (ImGui::BeginCombo("##Take", actor.Take[0] ? actor.Take : "Choose one")) {
            for (const actors::TakeInfo& take : takes)
                if (ImGui::Selectable(take.Name.c_str(), take.Name == actor.Take)) {
                    actors::SetTake(i, take.Name);
                    actor.On = true;
                }
            if (takes.empty())
                ImGui::TextColored(Dim, "Record a take first");
            ImGui::EndCombo();
        }
        int body = actor.Body + 1;
        Row("Wears");
        if (ImGui::Combo("##Wears", &body, "The fighter it was recorded from\0P1\0P2\0"))
            actor.Body = body - 1;
        float start = static_cast<float>(actor.Start);
        Row("Starts at", KeepButton());
        if (ImGui::DragFloat("##Starts at", &start, 0.01f, 0, 0, "%.2f s"))
            actor.Start = start;
        ImGui::SameLine();
        if (ImGui::SmallButton("Now"))
            actor.Start = game::Clock();
        Row("Speed");
        ImGui::SliderFloat("##Speed", &actor.Speed, 0.05f, 4.0f, "%.2fx", ImGuiSliderFlags_Logarithmic);
        Row("At the end");
        ImGui::Combo("##At the end", &actor.End, "Hold the last pose\0Loop\0Disappear\0");
        DragPlace("Place", actor.Location);
        DragPlace("Turn", actor.Rotation, 0.5f, "%.1f");
        Row("Size");
        ImGui::SliderFloat("##Size", &actor.Scale, 0.1f, 5.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
        const std::string status = actors::ActorStatus(i);
        if (!status.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, Warn);
            ImGui::TextWrapped("%s", status.c_str());
            ImGui::PopStyleColor();
        }
        ImGui::PopID();
    }
    ImGui::Separator();
    if (ImGui::SmallButton("+ Add an actor"))
        for (int i = 0; i < actors::MaxActors; ++i)
            if (!cast[i].On && !cast[i].Take[0] && !open[i]) {
                open[i] = true;
                cast[i].Start = game::Clock();
                break;
            }
    Help("An actor stands where the fighter stood when the take began until you move it: Place is where the take's first frame goes, and Here brings it in front of the camera. Its pose comes from the fight clock, so it's the same on every pass and rewinds with the replay. It wears the fighter's body and clothes as they are in this fight. Jackets and straps that the game moves with its cloth solver hang still on an actor. To have only actors in the picture, hide P1 and P2 under Look > Stage.");
}

void SceneTab()
{
    // The lists of what the game has loaded are long, so they're fetched now and then, not every frame.
    static std::vector<std::string> meshes, systems;
    static double listedAt = -10;
    if (ImGui::GetTime() - listedAt > 0.5) {
        meshes = spawner::MeshNames();
        systems = spawner::EffectNames();
        listedAt = ImGui::GetTime();
    }
    ImGui::TextColored(Dim, "Everything here is kept in look keys (L), so it can move over a shot.");
    ImGui::SetNextItemOpen(true, ImGuiCond_FirstUseEver);
    if (Group("Actors"))
        ActorsGroup();
    if (Group("Lights"))
        LightsGroup();
    if (Group("Props"))
        PropsGroup(meshes);
    if (Group("Effects"))
        EffectsGroup(systems);
}

// ReShade's techniques and the values of the effects that are on, for when the player has ReShade in the game.
void ReShadeGroup()
{
    ImGui::TextWrapped("%s", reshadelink::StatusText().c_str());
    if (!reshadelink::Found())
        return;
    static std::vector<reshadelink::Technique> techniques;
    static std::vector<reshadelink::Value> values;
    static double readAt = -10;
    if (ImGui::GetTime() - readAt > 0.25 && !ImGui::IsAnyItemActive()) {
        techniques = reshadelink::Techniques();
        values = reshadelink::Values();
        readAt = ImGui::GetTime();
    }
    for (reshadelink::Technique& technique : techniques) {
        bool on = technique.On;
        if (ImGui::Checkbox((technique.Name + "##" + technique.Effect).c_str(), &on)) {
            reshadelink::SetTechnique(technique.Effect, technique.Name, on);
            technique.On = on;
        }
    }
    std::string effect;
    for (reshadelink::Value& value : values) {
        if (value.Effect != effect) {
            effect = value.Effect;
            ImGui::TextColored(Dim, "%s", effect.c_str());
        }
        ImGui::PushID((value.Effect + "/" + value.Name).c_str());
        bool changed = false;
        Row(value.Label.c_str());
        const char* label = "##value";
        if (value.Kind == reshadelink::Kind::Bool) {
            bool on = value.Numbers[0] >= 0.5f;
            if (ImGui::Checkbox(label, &on)) {
                value.Numbers[0] = on ? 1.0f : 0.0f;
                changed = true;
            }
        } else if (value.Colour) {
            changed = value.Count >= 4 ? ImGui::ColorEdit4(label, value.Numbers, ImGuiColorEditFlags_NoInputs) : ImGui::ColorEdit3(label, value.Numbers, ImGuiColorEditFlags_NoInputs);
        } else if (value.Kind == reshadelink::Kind::Int) {
            int numbers[4];
            for (int i = 0; i < 4; ++i)
                numbers[i] = static_cast<int>(std::lround(value.Numbers[i]));
            const int low = static_cast<int>(value.Min), high = static_cast<int>(value.Max);
            changed = ImGui::SliderScalarN(label, ImGuiDataType_S32, numbers, value.Count, &low, &high);
            for (int i = 0; i < 4; ++i)
                value.Numbers[i] = static_cast<float>(numbers[i]);
        } else {
            changed = ImGui::SliderScalarN(label, ImGuiDataType_Float, value.Numbers, value.Count, &value.Min, &value.Max, "%.3f");
        }
        if (changed)
            reshadelink::SetValue(value.Effect, value.Name, value.Numbers);
        ImGui::PopID();
    }
    if (values.empty())
        ImGui::TextColored(Dim, "Switch a technique on to see its values.");
    Help("These are ReShade's own settings. Everything here is kept in a look key, so an effect can change over a shot: set it, add a look key (L), go to another moment, change it, add another. Recordings have ReShade's effects in them. Close ReShade's own menu before recording, or it's in the video too.");
}

// ----- RECORD tab

void RecordTab()
{
    recorder::Settings& s = recorder::Config();
    const bool busy = recorder::Active() || recorder::Finishing();

    if (ImGui::Button(recorder::Active() ? "Stop (F7)" : "Record (F7)", ImVec2(ImGui::GetContentRegionAvail().x, ImGui::GetFrameHeight() * 1.6f)))
        recorder::Toggle();
    if (recorder::Active())
        ImGui::TextColored(Accent, "Recording: %llu frames", static_cast<unsigned long long>(recorder::Frames()));
    else if (recorder::Finishing())
        ImGui::TextColored(Dim, "Finishing the file...");
    const std::string last = recorder::LastFile();
    if (!last.empty())
        ImGui::TextWrapped("Last: %s", std::filesystem::path(last).filename().string().c_str());
    const std::string error = recorder::LastError();
    if (!error.empty())
        ImGui::TextColored(Accent, "%s", error.c_str());

    ImGui::BeginDisabled(busy);

    Section("Output");
    Row("Framerate");
    ImGui::InputInt("##Framerate", &s.Fps, 0);
    s.Fps = std::clamp(s.Fps, 1, 1000);
    const auto& codecs = recorder::Codecs();
    s.CodecIndex = std::clamp(s.CodecIndex, 0, static_cast<int>(codecs.size()) - 1);
    Row("Format");
    if (ImGui::BeginCombo("##Format", codecs[s.CodecIndex].Label)) {
        for (int i = 0; i < static_cast<int>(codecs.size()); ++i)
            if (ImGui::Selectable(codecs[i].Label, i == s.CodecIndex))
                s.CodecIndex = i;
        ImGui::EndCombo();
    }
    ImGui::TextColored(Dim, "Folder: %s", s.OutputDir.string().c_str());
    if (ImGui::SmallButton("Open folder")) {
        std::error_code ignored;
        std::filesystem::create_directories(s.OutputDir, ignored);
        ShellExecuteW(nullptr, L"open", s.OutputDir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }

    Section("Picture size");
    {
        static const int sizes[][2] = { { 1280, 720 }, { 1920, 1080 }, { 2560, 1440 }, { 3840, 2160 }, { 5120, 2880 }, { 7680, 4320 } };
        int pictureWidth = 0, pictureHeight = 0;
        window::PictureSize(pictureWidth, pictureHeight);
        char window[64], current[64];
        snprintf(window, sizeof(window), "The game window (%d x %d)", pictureWidth, pictureHeight);
        const bool ownSize = s.CaptureWidth <= 0 || s.CaptureHeight <= 0;
        if (ownSize)
            strcpy_s(current, window);
        else
            snprintf(current, sizeof(current), "%d x %d", s.CaptureWidth, s.CaptureHeight);
        Row("Render size");
        if (ImGui::BeginCombo("##Render size", current)) {
            if (ImGui::Selectable(window, ownSize))
                s.CaptureWidth = s.CaptureHeight = 0;
            for (const auto& size : sizes) {
                char label[32];
                snprintf(label, sizeof(label), "%d x %d", size[0], size[1]);
                if (ImGui::Selectable(label, s.CaptureWidth == size[0] && s.CaptureHeight == size[1])) {
                    s.CaptureWidth = size[0];
                    s.CaptureHeight = size[1];
                }
            }
            ImGui::EndCombo();
        }
        Help("The size the game draws at while it records. It can be any size, bigger than your monitor included, on whichever monitor the game is on: the game is put in a window of that size for as long as the recording runs and gets its own window back afterwards. Bigger takes more graphics memory and longer per frame, which a recording doesn't mind.");
        int custom[2] = { s.CaptureWidth, s.CaptureHeight };
        Row("or your own");
        if (ImGui::InputInt2("##or your own", custom)) {
            s.CaptureWidth = std::clamp(custom[0], 0, 16384);
            s.CaptureHeight = std::clamp(custom[1], 0, 16384);
        }

        const bool asRendered = s.OutputWidth <= 0 || s.OutputHeight <= 0;
        if (asRendered)
            strcpy_s(current, "As rendered");
        else
            snprintf(current, sizeof(current), "%d x %d", s.OutputWidth, s.OutputHeight);
        Row("Video size");
        if (ImGui::BeginCombo("##Video size", current)) {
            if (ImGui::Selectable("As rendered", asRendered))
                s.OutputWidth = s.OutputHeight = 0;
            for (const auto& size : sizes) {
                char label[32];
                snprintf(label, sizeof(label), "%d x %d", size[0], size[1]);
                if (ImGui::Selectable(label, s.OutputWidth == size[0] && s.OutputHeight == size[1])) {
                    s.OutputWidth = size[0];
                    s.OutputHeight = size[1];
                }
            }
            ImGui::EndCombo();
        }
        Help("The size of the file. Rendering big and writing the video smaller gives cleaner edges than rendering at the small size.");
        int videoWidth = 0, videoHeight = 0;
        recorder::VideoSize(pictureWidth, pictureHeight, videoWidth, videoHeight);
        ImGui::TextColored(Dim, "The video will be %d x %d", videoWidth, videoHeight);
    }

    Section("Streams");
    ImGui::TextWrapped("Each one on makes its own file. With several on, each moment renders once per stream, and the files line up frame for frame.");
    const char* looks = "Picture\0Greenscreen\0Stage only\0Fighters on black\0Depth\0Effects on black\0";
    for (int i = 0; i < recorder::MaxStreams; ++i) {
        recorder::Stream& stream = s.Streams[i];
        ImGui::PushID(i);
        ImGui::Checkbox("##on", &stream.Enabled);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.4f);
        ImGui::InputText("##name", stream.Name, sizeof(stream.Name));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-1);
        int look = static_cast<int>(stream.Look);
        if (ImGui::Combo("##look", &look, looks))
            stream.Look = static_cast<recorder::Look>(look);
        ImGui::PopID();
    }
    Row("Settle frames");
    ImGui::SliderInt("##Settle frames", &s.SettleFrames, 0, 16);
    Help("Frames to wait after switching stream, so the new look is really on screen. Raise it if streams bleed into each other. Lower is faster.");

    Section("Preview a look");
    int preview = static_cast<int>(s.Preview);
    Row("Look now");
    if (ImGui::Combo("##Look now", &preview, "Picture\0Greenscreen\0Stage only\0Fighters on black\0Depth\0Effects on black\0"))
        s.Preview = static_cast<recorder::Look>(preview);
    int keyColour = s.BlueKey ? 1 : 0;
    Row("Screen colour");
    if (ImGui::Combo("##Screen colour", &keyColour, "Green\0Blue\0"))
        s.BlueKey = keyColour == 1;
    Help("The colour behind the fighters in the Greenscreen look, on screen and in the video.");
    Row("Depth near");
    ImGui::SliderFloat("##Depth near", &s.DepthNear, 0.0f, 2000.0f, "%.0f cm");
    Help("How close to the camera the depth look is pure white. Anything closer is white too.");
    Row("Depth far");
    ImGui::SliderFloat("##Depth far", &s.DepthFar, 100.0f, 20000.0f, "%.0f cm", ImGuiSliderFlags_Logarithmic);
    Help("How far from the camera the depth look fades to black.");
    Row("Depth curve");
    ImGui::SliderFloat("##Depth curve", &s.DepthCurve, 0.25f, 4.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
    Help("Above 1 the grey falls off quickly behind the near distance, and below 1 it holds bright for longer.");
    const recorder::Pass shownPass = recorder::ActivePass();
    if ((shownPass == recorder::Pass::Depth || shownPass == recorder::Pass::Greenscreen) && !depth::Working()) {
        ImGui::TextColored(Dim, "Can't read the game's depth buffer yet.");
        Row("Key cut");
        ImGui::SliderInt("##Key cut", &s.GreenDarkness, 2, 80);
        Help("Without the depth buffer, the video keys on how dark the background is. Dark clothing at a fighter's edge gets eaten if this is too high.");
    }

    Section("Motion blur");
    Row("Samples per frame");
    ImGui::SliderInt("##Samples per frame", &s.MotionSamples, 1, 64);
    Help("1 = off. Each video frame averages this many real renders, so it takes that many times longer. 16 to 32 looks like film.");
    Row("Shutter");
    ImGui::SliderFloat("##Shutter", &s.ShutterDegrees, 1.0f, 360.0f, "%.0f deg");
    Help("180 is the film standard: open for half of each frame's time.");

    Section("Range");
    ImGui::Checkbox("Record the timeline's range by itself", &s.UseRange);
    Help("Starts when the fight clock reaches the range and stops at the end of it. Drag the range on the timeline.");

    ImGui::EndDisabled();

    Section("Audio pass");
    ImGui::TextWrapped("Video has no sound. Play the same part again at 1x and this captures only the game's sound to a WAV.");
    if (ImGui::Button(audio::Running() ? "Stop audio" : "Record audio now")) {
        if (audio::Running()) {
            audio::Stop();
        } else {
            char name[64];
            const std::time_t now = std::time(nullptr);
            std::tm local {};
            localtime_s(&local, &now);
            std::strftime(name, sizeof(name), "tekken8mvm_%Y%m%d_%H%M%S_audio.wav", &local);
            audio::Start(s.OutputDir / name);
        }
    }
    ImGui::SameLine();
    ImGui::Checkbox("Over the range", &audio::Config().Armed);
    if (audio::Running())
        ImGui::TextColored(Accent, "Capturing: %.1f s%s", audio::SecondsCaptured(), audio::HeardSound() ? "" : " (silent so far)");
    const std::string audioError = audio::LastError();
    if (!audioError.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, Accent);
        ImGui::TextWrapped("%s", audioError.c_str());
        ImGui::PopStyleColor();
    } else if (!audio::LastFile().empty() && !audio::Running())
        ImGui::TextColored(Dim, "Last: %s", std::filesystem::path(audio::LastFile()).filename().string().c_str());

    Section("System");
    const game::HookStatus& h = game::Hooks();
    const auto status = [](const char* label, bool ok) { ImGui::TextColored(ok ? Good : Accent, "%s  %s", ok ? "OK " : "OFF", label); };
    status("Time control", h.Tick);
    status("Camera", h.Camera);
    status("Character blending", h.Actors);
    status("Bone blending", h.Bones);
    status("Fight detection", h.FightStart);
    if (postfx::Checked())
        status("Depth of field and colour", postfx::Available());
    else
        ImGui::TextColored(Dim, "--   Depth of field and colour (not used yet)");
    const int steam = guard::SteamState();
    ImGui::TextColored(steam == 0 ? Good : (steam == 1 ? Accent : Dim), "Steam: %s", steam == 0 ? "Offline Mode" : (steam == 1 ? "signed in" : "not started yet"));
    ImGui::Text("Internet connections from the game: %d", guard::LastConnectionCount());
    ImGui::Text("Connections stopped by the network block: %d", guard::BlockedAttempts());
}

// ----- Timeline

struct TimelineState {
    double Start = 0, End = 30;
    bool Follow = true;
    int SelectedCamera = -1;
    int DragCamera = -1;
    int DragSpeed = -1;
    int DragLook = -1;
    int DragRange = 0; // 1 = start handle, 2 = end handle
    int DragSceneKind = -1; // 0 = an actor, 1 = an effect
    int DragSceneIndex = -1;
    double DragSceneOffset = 0;
} g_timeline;

float TimeToX(double time, float left, float width)
{
    return left + static_cast<float>((time - g_timeline.Start) / (g_timeline.End - g_timeline.Start)) * width;
}

double XToTime(float x, float left, float width)
{
    return g_timeline.Start + static_cast<double>(x - left) / width * (g_timeline.End - g_timeline.Start);
}

float SpeedToY(double speed, float top, float height)
{
    const double u = (std::log(speed) - std::log(MinSpeed)) / (std::log(MaxSpeed) - std::log(MinSpeed));
    return top + height - static_cast<float>(u) * height;
}

double YToSpeed(float y, float top, float height)
{
    const double u = std::clamp(static_cast<double>(top + height - y) / height, 0.0, 1.0);
    return std::exp(std::log(MinSpeed) + u * (std::log(MaxSpeed) - std::log(MinSpeed)));
}

// Guides over the picture. They're drawn with the editor, after the frame is captured, so they never end up in a recording.
void DrawThirds(ImDrawList* draw, ImVec2 min, ImVec2 max)
{
    const scene::Settings& s = scene::Config();
    if (s.Frame == scene::Frame::Fixed) {
        // Inside the bars a fixed aspect ratio adds.
        const float width = max.x - min.x, height = max.y - min.y;
        if (s.AspectRatio > width / height) {
            const float inset = (height - width / s.AspectRatio) * 0.5f;
            min.y += inset;
            max.y -= inset;
        } else {
            const float inset = (width - height * s.AspectRatio) * 0.5f;
            min.x += inset;
            max.x -= inset;
        }
    }
    const ImU32 colour = IM_COL32(255, 255, 255, 110);
    for (int i = 1; i <= 2; ++i) {
        const float x = min.x + (max.x - min.x) * i / 3.0f, y = min.y + (max.y - min.y) * i / 3.0f;
        draw->AddLine({ x, min.y }, { x, max.y }, colour);
        draw->AddLine({ min.x, y }, { max.x, y }, colour);
    }
}

void DrawDiamond(ImDrawList* draw, ImVec2 c, float r, ImU32 fill, ImU32 outline)
{
    draw->AddQuadFilled({ c.x, c.y - r }, { c.x + r, c.y }, { c.x, c.y + r }, { c.x - r, c.y }, fill);
    draw->AddQuad({ c.x, c.y - r }, { c.x + r, c.y }, { c.x, c.y + r }, { c.x - r, c.y }, outline, 1.0f);
}

void Timeline(ImVec2 position, ImVec2 size)
{
    ImGui::SetNextWindowPos(position);
    ImGui::SetNextWindowSize(size);
    ImGui::Begin("##timeline", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollWithMouse);
    TimelineState& v = g_timeline;
    recorder::Settings& rec = recorder::Config();

    // Header row
    if (ImGui::SmallButton(ICON_FA_VIDEO "  Camera key (K)"))
        camera::AddKeyAtCurrentView();
    ImGui::SameLine();
    if (ImGui::SmallButton(ICON_FA_SLIDERS "  Look key (L)"))
        presets::AddLookKey();
    Help("Keeps everything as it is at this moment: every Look setting, the lights, props, effects and actors on the Scene tab, the attached camera, and ReShade's effects. With two or more, sliders, colours, places and sizes move from one key to the next as the replay plays, and switches change at the key. Set things up, add a key, go to another moment, change them, add another. The camera path and the timescale have lanes of their own.");
    ImGui::SameLine();
    {
        const std::vector<cine::CameraKey> keys = camera::Keys();
        ImGui::BeginDisabled(keys.size() < 2 || recorder::Active() || recorder::Finishing());
        if (ImGui::SmallButton("Preview shot"))
            camera::PlayShot();
        Help("Goes to the first camera key, flies the path and stops on the last one. A click on the ruler goes anywhere else.");
        ImGui::SameLine();
        if (ImGui::SmallButton("Record shot")) {
            camera::PlayShot();
            recorder::RecordRange(keys.front().Time, keys.back().Time);
            Notice("Going to the first key to record from there");
        }
        Help("The same, recording from the first key's exact frame to the last key's, with the streams set up in the Record tab.");
        ImGui::EndDisabled();
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear camera"))
        camera::ClearKeys();
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear speed")) {
        game::SetSpeedKeys({});
        game::Time().UseSpeedRamp = false;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear look"))
        presets::ClearLookKeys();
    ImGui::SameLine();
    if (ImGui::SmallButton("Zero clock here"))
        game::ZeroClock();
    Help("Keys are timed on this clock. Zero it at the same moment in a replay to line a saved path back up.");
    ImGui::SameLine();
    if (ImGui::SmallButton("Range start here"))
        rec.RangeIn = game::Clock();
    ImGui::SameLine();
    if (ImGui::SmallButton("Range end here"))
        rec.RangeOut = game::Clock();
    ImGui::SameLine();
    ImGui::Checkbox("Follow", &v.Follow);

    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float font = ImGui::GetFontSize();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float labelWidth = font * 8.0f;
    const float left = origin.x + labelWidth;
    const float width = std::max(position.x + size.x - left - font, 10.0f);
    const float available = position.y + size.y - origin.y - font * 0.5f;
    const float rulerHeight = font * 1.4f;
    const float cameraHeight = std::max(font * 1.5f, available * 0.25f);
    const float lookHeight = std::max(font * 1.3f, available * 0.12f);
    const float rangeHeight = font * 1.2f;
    // The scene lane is only there when something on the stage has a place in time: an actor, or an effect that runs on the timeline.
    bool sceneLane = false;
    for (const actors::Actor& actor : actors::Cast())
        sceneLane = sceneLane || (actor.On && actor.Take[0]);
    for (const spawner::Effect& effect : spawner::Effects())
        sceneLane = sceneLane || (effect.On && effect.OnTimeline);
    const float sceneHeight = sceneLane ? std::max(font * 1.3f, available * 0.12f) : 0.0f;
    const float speedHeight = std::max(available - rulerHeight - cameraHeight - lookHeight - sceneHeight - rangeHeight, font * 1.5f);

    const float rulerTop = origin.y;
    const float cameraTop = rulerTop + rulerHeight;
    const float lookTop = cameraTop + cameraHeight;
    const float sceneTop = lookTop + lookHeight;
    const float speedTop = sceneTop + sceneHeight;
    const float rangeTop = speedTop + speedHeight;
    const float bottom = rangeTop + rangeHeight;
    g_layout.Left = left;
    g_layout.Width = width;
    g_layout.RulerTop = rulerTop;
    g_layout.CameraTop = cameraTop;
    g_layout.LookTop = lookTop;
    g_layout.SpeedTop = speedTop;
    g_layout.RangeTop = rangeTop;
    g_layout.Bottom = bottom;

    // Keep the playhead in view, and take the input for the whole strip.
    const double clock = game::Clock();
    if (v.Follow && (clock < v.Start || clock > v.End)) {
        const double span = v.End - v.Start;
        v.Start = std::max(clock - span * 0.1, 0.0);
        v.End = v.Start + span;
    }
    ImGui::SetCursorScreenPos({ origin.x, origin.y });
    ImGui::InvisibleButton("##strip", { position.x + size.x - origin.x, bottom - origin.y }, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
    const bool hovered = ImGui::IsItemHovered();
    const ImVec2 mouse = ImGui::GetMousePos();
    const bool inLanes = mouse.x >= left && mouse.x <= left + width;

    if (hovered && inLanes && ImGui::GetIO().MouseWheel != 0) {
        const double focus = XToTime(mouse.x, left, width);
        const double scale = std::pow(0.85, ImGui::GetIO().MouseWheel);
        const double span = std::clamp((v.End - v.Start) * scale, 0.5, 3600.0);
        const double ratio = (focus - v.Start) / (v.End - v.Start);
        v.Start = focus - span * ratio;
        v.End = v.Start + span;
    }
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) {
        const double shift = -ImGui::GetIO().MouseDelta.x / width * (v.End - v.Start);
        v.Start += shift;
        v.End += shift;
        v.Follow = false;
    }

    // Lane backgrounds and names
    const ImU32 laneA = Hex(pal::Surface), laneB = Hex(Mix(pal::Surface, pal::Bg, 0.5f)), text = Hex(pal::Text), dim = Hex(pal::Muted);
    draw->AddRectFilled({ left, rulerTop }, { left + width, cameraTop }, Hex(pal::Bg));
    draw->AddRectFilled({ left, cameraTop }, { left + width, lookTop }, laneA);
    draw->AddRectFilled({ left, lookTop }, { left + width, sceneTop }, laneB);
    if (sceneLane)
        draw->AddRectFilled({ left, sceneTop }, { left + width, speedTop }, laneA);
    draw->AddRectFilled({ left, speedTop }, { left + width, rangeTop }, sceneLane ? laneB : laneA);
    draw->AddRectFilled({ left, rangeTop }, { left + width, bottom }, sceneLane ? laneA : laneB);
    // Each lane's name on the left, with its icon, in the semi-bold face.
    const auto laneName = [&](float top, float height, const char* icon, const char* name) {
        const float y = top + std::max((height - font) * 0.5f, 0.0f);
        draw->AddText({ origin.x + font * 0.3f, y }, dim, icon);
        draw->AddText(BoldFont(), font, { origin.x + font * 1.7f, y }, text, name);
    };
    laneName(cameraTop, std::min(lookTop - cameraTop, font * 1.6f), ICON_FA_VIDEO, "Camera");
    laneName(lookTop, sceneTop - lookTop, ICON_FA_SLIDERS, "Look");
    if (sceneLane)
        laneName(sceneTop, speedTop - sceneTop, ICON_FA_PERSON, "Scene");
    laneName(speedTop, std::min(rangeTop - speedTop, font * 1.6f), ICON_FA_GAUGE_HIGH, "Timescale");
    laneName(rangeTop, bottom - rangeTop, ICON_FA_ARROWS_LEFT_RIGHT_TO_LINE, "Range");
    draw->PushClipRect({ left, rulerTop }, { left + width, bottom }, true);

    // Ruler: labels in seconds with the game's 60 Hz ticks beside them.
    const double span = v.End - v.Start;
    const double wanted = span / std::max(width / (font * 7.0f), 1.0f);
    double step = 0.1;
    for (const double candidate : { 0.1, 0.25, 0.5, 1.0, 2.0, 5.0, 10.0, 30.0, 60.0, 120.0, 300.0 })
        if (candidate >= wanted) {
            step = candidate;
            break;
        } else {
            step = candidate;
        }
    for (double t = std::floor(v.Start / step) * step; t <= v.End; t += step) {
        const float x = TimeToX(t, left, width);
        draw->AddLine({ x, cameraTop }, { x, bottom }, Hex(pal::Border, 150));
        draw->AddLine({ x, rulerTop + rulerHeight * 0.45f }, { x, cameraTop }, dim);
        // Small marks between the numbered ones, as a ruler has.
        for (int mark = 1; mark < 5; ++mark) {
            const float between = TimeToX(t + step * mark / 5.0, left, width);
            draw->AddLine({ between, rulerTop + rulerHeight * 0.75f }, { between, cameraTop }, Hex(pal::Border));
        }
        draw->AddText({ x + 4, rulerTop }, dim, TimeText(t, step < 1.0 ? 2 : 0).c_str());
    }

    // Timescale lane: a guide at 1x, then the ramp's curve and keys.
    {
        const float oneX = SpeedToY(1.0, speedTop, speedHeight);
        draw->AddLine({ left, oneX }, { left + width, oneX }, Hex(pal::Border));
        draw->AddText({ left + 3, oneX - font }, dim, "1x");
        auto keys = game::SpeedKeys();
        if (!keys.empty()) {
            std::vector<ImVec2> points;
            for (float x = left; x <= left + width; x += 3.0f)
                points.push_back({ x, SpeedToY(std::clamp(cine::SampleSpeed(keys, XToTime(x, left, width)), MinSpeed, MaxSpeed), speedTop, speedHeight) });
            draw->AddPolyline(points.data(), static_cast<int>(points.size()), Hex(pal::Warning), 0, 2.0f);
        }
        int hit = -1;
        for (int i = 0; i < static_cast<int>(keys.size()); ++i) {
            const ImVec2 at { TimeToX(keys[i].Time, left, width), SpeedToY(keys[i].Speed, speedTop, speedHeight) };
            if (std::fabs(mouse.x - at.x) < font * 0.5f && std::fabs(mouse.y - at.y) < font * 0.5f)
                hit = i;
            draw->AddCircleFilled(at, font * 0.32f, i == v.DragSpeed ? IM_COL32_WHITE : Hex(pal::Warning));
        }
        const bool inSpeed = hovered && inLanes && mouse.y >= speedTop && mouse.y < rangeTop;
        if (inSpeed && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            if (hit >= 0) {
                v.DragSpeed = hit;
            } else {
                keys.push_back({ XToTime(mouse.x, left, width), YToSpeed(mouse.y, speedTop, speedHeight) });
                const double added = keys.back().Time;
                std::sort(keys.begin(), keys.end(), [](const cine::SpeedKey& a, const cine::SpeedKey& b) { return a.Time < b.Time; });
                for (int i = 0; i < static_cast<int>(keys.size()); ++i)
                    if (keys[i].Time == added)
                        v.DragSpeed = i;
                game::SetSpeedKeys(keys);
                game::Time().UseSpeedRamp = true;
            }
        }
        if (inSpeed && ImGui::IsMouseClicked(ImGuiMouseButton_Right) && hit >= 0) {
            keys.erase(keys.begin() + hit);
            game::SetSpeedKeys(keys);
        }
        if (v.DragSpeed >= 0) {
            if (ImGui::IsMouseDown(ImGuiMouseButton_Left) && v.DragSpeed < static_cast<int>(keys.size())) {
                keys[v.DragSpeed].Time = XToTime(mouse.x, left, width);
                keys[v.DragSpeed].Speed = YToSpeed(mouse.y, speedTop, speedHeight);
                const cine::SpeedKey moved = keys[v.DragSpeed];
                std::sort(keys.begin(), keys.end(), [](const cine::SpeedKey& a, const cine::SpeedKey& b) { return a.Time < b.Time; });
                for (int i = 0; i < static_cast<int>(keys.size()); ++i)
                    if (keys[i].Time == moved.Time && keys[i].Speed == moved.Speed)
                        v.DragSpeed = i;
                game::SetSpeedKeys(keys);
                char tip[32];
                snprintf(tip, sizeof(tip), "%.3gx", moved.Speed);
                ImGui::SetTooltip("%s", tip);
            } else {
                v.DragSpeed = -1;
            }
        }
    }

    // Camera lane
    {
        const auto keys = camera::Keys();
        const float mid = cameraTop + cameraHeight * 0.5f;
        if (keys.size() >= 2)
            draw->AddLine({ TimeToX(keys.front().Time, left, width), mid }, { TimeToX(keys.back().Time, left, width), mid }, Hex(pal::Muted), 2.0f);
        int hit = -1;
        const float radius = std::min(font * 0.5f, cameraHeight * 0.4f);
        for (int i = 0; i < static_cast<int>(keys.size()); ++i) {
            const ImVec2 at { TimeToX(keys[i].Time, left, width), mid };
            if (std::fabs(mouse.x - at.x) < radius + 2 && std::fabs(mouse.y - at.y) < cameraHeight * 0.5f)
                hit = i;
            DrawDiamond(draw, at, radius, i == v.SelectedCamera ? IM_COL32_WHITE : Hex(pal::Accent), Hex(pal::Bg));
        }
        const bool inCamera = hovered && inLanes && mouse.y >= cameraTop && mouse.y < lookTop;
        if (inCamera && hit >= 0) {
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                camera::GoToKey(hit);
            } else if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                v.SelectedCamera = hit;
                v.DragCamera = hit;
            }
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                camera::RemoveKey(hit);
                v.SelectedCamera = -1;
            }
            ImGui::SetTooltip("Key %d at %.3f s (tick %d)\nDrag to move, double-click to go there, right-click to delete", hit + 1, keys[hit].Time,
                              static_cast<int>(std::lround(keys[hit].Time * 60)));
        }
        if (v.DragCamera >= 0) {
            if (ImGui::IsMouseDown(ImGuiMouseButton_Left) && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 2.0f)) {
                v.DragCamera = static_cast<int>(camera::MoveKey(v.DragCamera, std::max(XToTime(mouse.x, left, width), 0.0)));
                v.SelectedCamera = v.DragCamera;
            } else if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                v.DragCamera = -1;
            }
        }
    }

    // Look lane: each key is every Look setting at that moment.
    {
        const std::vector<double> times = presets::LookKeyTimes();
        const float mid = lookTop + lookHeight * 0.5f;
        if (times.size() >= 2)
            draw->AddLine({ TimeToX(times.front(), left, width), mid }, { TimeToX(times.back(), left, width), mid }, Hex(pal::Blue, 140), 2.0f);
        int hit = -1;
        const float radius = std::min(font * 0.45f, lookHeight * 0.4f);
        for (int i = 0; i < static_cast<int>(times.size()); ++i) {
            const ImVec2 at { TimeToX(times[i], left, width), mid };
            if (std::fabs(mouse.x - at.x) < radius + 2 && std::fabs(mouse.y - at.y) < lookHeight * 0.5f)
                hit = i;
            DrawDiamond(draw, at, radius, i == v.DragLook ? IM_COL32_WHITE : Hex(pal::Blue), Hex(pal::Bg));
        }
        const bool inLook = hovered && inLanes && mouse.y >= lookTop && mouse.y < sceneTop;
        if (inLook && hit >= 0) {
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                if (times[hit] > clock || replay::CanJump())
                    game::SeekTo(times[hit]);
            } else if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                v.DragLook = hit;
            }
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                presets::RemoveLookKey(hit);
                v.DragLook = -1;
            }
            ImGui::SetTooltip("Look key %d at %.3f s\nDrag to move, double-click to go there, right-click to delete.\nTo change it, go there, change the settings and add a look key again.", hit + 1, times[hit]);
        }
        if (v.DragLook >= 0) {
            if (ImGui::IsMouseDown(ImGuiMouseButton_Left) && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 2.0f))
                v.DragLook = static_cast<int>(presets::MoveLookKey(v.DragLook, std::max(XToTime(mouse.x, left, width), 0.0)));
            else if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
                v.DragLook = -1;
        }
    }

    // Scene lane: a bar for each actor, as long as its take plays for, and one for each effect on the timeline. Drag a bar to move its start.
    if (sceneLane) {
        const std::vector<actors::TakeInfo> takes = actors::Takes();
        const bool inScene = hovered && inLanes && mouse.y >= sceneTop && mouse.y < speedTop;
        const float barTop = sceneTop + 2, barBottom = speedTop - 2;
        int hitKind = -1, hitIndex = -1;
        const auto bar = [&](int kind, int index, double start, double length, ImU32 colour, const char* letter) {
            const float a = TimeToX(start, left, width);
            const float b = length > 0 ? TimeToX(start + length, left, width) : left + width; // no end: it runs on to the edge
            draw->AddRectFilled({ a, barTop }, { std::max(b, a + 3.0f), barBottom }, colour);
            char label[16];
            snprintf(label, sizeof(label), "%s%d", letter, index + 1);
            draw->AddText({ a + 3, barTop }, IM_COL32(255, 255, 255, 230), label);
            if (inScene && mouse.x >= a && mouse.x <= std::max(b, a + 3.0f) + 2) {
                hitKind = kind;
                hitIndex = index;
            }
        };
        for (int i = 0; i < actors::MaxActors; ++i) {
            const actors::Actor& actor = actors::Cast()[i];
            if (!actor.On || !actor.Take[0])
                continue;
            double length = 1.0;
            for (const actors::TakeInfo& take : takes)
                if (take.Name == actor.Take)
                    length = take.Frames / 60.0 / std::max(static_cast<double>(actor.Speed), 0.01);
            bar(0, i, actor.Start, length, Hex(pal::Blue), "A");
        }
        for (int i = 0; i < spawner::MaxEffects; ++i) {
            const spawner::Effect& effect = spawner::Effects()[i];
            if (effect.On && effect.OnTimeline)
                bar(1, i, effect.Start, effect.Length > 0 ? effect.Length / std::max(static_cast<double>(effect.Rate), 0.01) : 0.0, Hex(Mix(pal::Warning, pal::Bg, 0.25f)), "E");
        }
        if (hitKind >= 0) {
            ImGui::SetTooltip(hitKind == 0 ? "Actor %d: drag to move where its take starts" : "Effect %d: drag to move where it starts", hitIndex + 1);
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                v.DragSceneKind = hitKind;
                v.DragSceneIndex = hitIndex;
                const double start = hitKind == 0 ? actors::Cast()[hitIndex].Start : spawner::Effects()[hitIndex].Start;
                v.DragSceneOffset = XToTime(mouse.x, left, width) - start;
            }
        }
    }
    if (v.DragSceneKind >= 0) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            if (ImGui::IsMouseDragging(ImGuiMouseButton_Left, 2.0f)) {
                // On a game step, so the actor starts with the fight's frames and not between two.
                const double start = std::round((XToTime(mouse.x, left, width) - v.DragSceneOffset) * 60.0) / 60.0;
                if (v.DragSceneKind == 0)
                    actors::Cast()[v.DragSceneIndex].Start = start;
                else
                    spawner::Effects()[v.DragSceneIndex].Start = start;
            }
        } else {
            v.DragSceneKind = -1;
        }
    }

    // Range lane
    {
        const float a = TimeToX(rec.RangeIn, left, width), b = TimeToX(rec.RangeOut, left, width);
        // A red band when it's armed to record by itself, grey when it only marks a stretch. Its edges run up through the lanes.
        const ImU32 edge = rec.UseRange ? Hex(pal::Red) : Hex(pal::Muted);
        draw->AddRectFilled({ a, rangeTop + 2 }, { b, bottom - 2 }, rec.UseRange ? Hex(pal::Red, 90) : Hex(pal::Muted, 60));
        draw->AddLine({ a, cameraTop }, { a, rangeTop }, Hex(rec.UseRange ? pal::Red : pal::Muted, 70));
        draw->AddLine({ b, cameraTop }, { b, rangeTop }, Hex(rec.UseRange ? pal::Red : pal::Muted, 70));
        draw->AddRectFilled({ a - 1, rangeTop }, { a + 2, bottom }, edge);
        draw->AddRectFilled({ b - 1, rangeTop }, { b + 2, bottom }, edge);
        const bool inRange = hovered && inLanes && mouse.y >= rangeTop && mouse.y <= bottom;
        if (inRange && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            v.DragRange = std::fabs(mouse.x - a) <= std::fabs(mouse.x - b) ? 1 : 2;
        if (v.DragRange && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            const double t = std::max(XToTime(mouse.x, left, width), 0.0);
            if (v.DragRange == 1)
                rec.RangeIn = std::min(t, rec.RangeOut - 0.01);
            else
                rec.RangeOut = std::max(t, rec.RangeIn + 0.01);
        } else {
            v.DragRange = 0;
        }
    }

    // Clicking the ruler takes the fight to that point, either way.
    if (hovered && inLanes && mouse.y < cameraTop && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        const double target = XToTime(mouse.x, left, width);
        if (target > clock || replay::CanJump()) {
            game::SeekTo(target);
            Notice(target > clock ? "Going ahead..." : "Rewinding...");
        } else {
            Notice("Rewinding works in a replay. Practice mode only goes forward.");
        }
    }

    // Where the replay's rounds start and where it ends.
    for (const replay::SavedPoint& point : replay::SavedPoints()) {
        if (point.Type != replay::TypeRoundStart && point.Type != replay::TypeMatchEnd)
            continue;
        const float x = TimeToX(point.Frame * game::StepSeconds - game::ClockZero(), left, width);
        draw->AddLine({ x, rulerTop }, { x, bottom }, Hex(pal::Warning, 150));
        char label[16];
        if (point.Type == replay::TypeRoundStart)
            snprintf(label, sizeof(label), "R%d", point.Round);
        else
            snprintf(label, sizeof(label), "End");
        draw->AddText({ x + 4, rulerTop + rulerHeight }, Hex(pal::Warning), label);
    }

    // Playhead
    const float head = TimeToX(clock, left, width);
    draw->AddLine({ head, rulerTop }, { head, bottom }, Hex(pal::Accent), 2.0f);
    draw->AddTriangleFilled({ head - font * 0.35f, rulerTop }, { head + font * 0.35f, rulerTop }, { head, rulerTop + font * 0.5f }, Hex(pal::Accent));
    draw->PopClipRect();

    ImGui::End();
}

// ----- Playback bar

void ControlBar(ImVec2 position, ImVec2 size)
{
    ImGui::SetNextWindowPos(position);
    ImGui::SetNextWindowSize(size);
    ImGui::Begin("##controls", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus);
    game::TimeControl& t = game::Time();
    const float height = ImGui::GetFrameHeight();
    ImGui::SetCursorPosY((size.y - height) * 0.5f);
    const ImVec2 button { std::floor(height * 1.5f), height };
    const float gap = 2;

    // A frame back, play or pause, a frame on.
    ImGui::BeginDisabled(!replay::CanJump());
    if (ImGui::Button(ICON_FA_BACKWARD_STEP "##frameback", button))
        game::SeekTo(std::max(game::Clock() - game::StepSeconds, 0.0));
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("A frame back (,)");
    ImGui::SameLine(0, gap);
    if (ImGui::Button(t.Paused ? ICON_FA_PLAY "##playpause" : ICON_FA_PAUSE "##playpause", button))
        t.Paused = !t.Paused;
    ImGui::SetItemTooltip(t.Paused ? "Play (Space)" : "Pause (Space)");
    g_layout.PlayButton = { (ImGui::GetItemRectMin().x + ImGui::GetItemRectMax().x) * 0.5f, (ImGui::GetItemRectMin().y + ImGui::GetItemRectMax().y) * 0.5f };
    ImGui::SameLine(0, gap);
    if (ImGui::Button(ICON_FA_FORWARD_STEP "##frameon", button))
        game::StepOneFrame();
    ImGui::SetItemTooltip("A frame on (.)");

    // Where the replay is, and how long it is when the game has told us where it ends.
    const double clock = game::Clock();
    const std::vector<replay::SavedPoint> points = replay::SavedPoints();
    std::string time = TimeText(clock);
    for (const replay::SavedPoint& point : points)
        if (point.Type == replay::TypeMatchEnd)
            time += " / " + TimeText(point.Frame * game::StepSeconds - game::ClockZero());
    ImGui::SameLine();
    {
        const ImVec2 at = ImGui::GetCursorScreenPos();
        ImGui::PushFont(BoldFont());
        const float width = ImGui::CalcTextSize(time.c_str()).x + ImGui::GetStyle().FramePadding.x * 4;
        ImGui::GetWindowDrawList()->AddRectFilled(at, { at.x + width, at.y + height }, ImGui::GetColorU32(ImGuiCol_FrameBg));
        ImGui::SetCursorScreenPos({ at.x + ImGui::GetStyle().FramePadding.x * 2, at.y });
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(time.c_str());
        ImGui::PopFont();
        ImGui::SameLine(0, 0);
        ImGui::SetCursorScreenPos({ at.x + width, at.y });
        ImGui::Dummy({ 0, height });
    }

    ImGui::SameLine();
    ImGui::SetNextItemWidth(size.x / 9);
    float speed = static_cast<float>(t.Speed);
    ImGui::BeginDisabled(t.UseSpeedRamp);
    if (ImGui::SliderFloat("##timescale", &speed, static_cast<float>(MinSpeed), static_cast<float>(MaxSpeed), "%.3f", ImGuiSliderFlags_Logarithmic))
        t.Speed = speed;
    g_layout.SliderMin = ImGui::GetItemRectMin();
    g_layout.SliderMax = ImGui::GetItemRectMax();
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Timescale");
    for (const float preset : { 0.01f, 0.1f, 0.25f, 0.5f, 1.0f }) {
        ImGui::SameLine(0, gap);
        char label[16];
        snprintf(label, sizeof(label), "%gx", preset);
        if (ImGui::Button(label)) {
            t.Speed = preset;
            t.UseSpeedRamp = false;
        }
        g_layout.OneX = { (ImGui::GetItemRectMin().x + ImGui::GetItemRectMax().x) * 0.5f, (ImGui::GetItemRectMin().y + ImGui::GetItemRectMax().y) * 0.5f };
    }

    // Jump to the start of a round. Round one has no point of its own: it starts at the first saved point.
    int roundsShown = 0;
    const auto roundButton = [&](int round, uint32_t frame) {
        ImGui::SameLine(0, gap);
        ++roundsShown;
        char label[16];
        snprintf(label, sizeof(label), "R%d", round);
        if (ImGui::Button(label))
            game::SeekToRound(frame);
    };
    bool hasFirstRound = false;
    for (const replay::SavedPoint& point : points)
        hasFirstRound = hasFirstRound || (point.Type == replay::TypeRoundStart && point.Round <= 1);
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_FLAG "  Start"))
        game::SeekToStart();
    ++roundsShown;
    if (!points.empty() && !hasFirstRound)
        roundButton(1, points.front().Frame);
    for (const replay::SavedPoint& point : points)
        if (point.Type == replay::TypeRoundStart)
            roundButton(point.Round, point.Frame);

    const game::Rates rates = game::MeasuredRates();
    char info[160];
    snprintf(info, sizeof(info), "tick %d   %.1f ticks/s   %.0f fps%s", static_cast<int>(std::lround(clock * 60)), rates.TicksPerSecond, rates.FramesPerSecond,
             game::Seeking() ? "   running ahead" : "");
    const char* recording = recorder::Active() ? ICON_FA_CIRCLE "  REC   " : "";
    const float textWidth = ImGui::CalcTextSize(info).x + ImGui::CalcTextSize(recording).x;
    ImGui::SameLine(size.x - textWidth - ImGui::GetStyle().WindowPadding.x);
    ImGui::AlignTextToFramePadding();
    if (recording[0]) {
        ImGui::TextColored(Accent, "%s", recording);
        ImGui::SameLine(0, 0);
    }
    ImGui::TextColored(Dim, "%s", info);
    ImGui::End();
}

void MenuBar()
{
    if (!ImGui::BeginMainMenuBar())
        return;
    bool wantReset = false;
    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("Hide editor", "Insert"))
            overlay::ToggleMenu();
        if (ImGui::MenuItem("Undo", "Ctrl+Z"))
            Notice(presets::Undo() ? "Undone" : "Nothing to undo");
        if (ImGui::MenuItem("Redo", "Ctrl+Y"))
            Notice(presets::Redo() ? "Redone" : "Nothing to redo");
        ImGui::Separator();
        if (ImGui::MenuItem("Reset the mod..."))
            wantReset = true;
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
        ImGui::MenuItem("Hide the HUD", "F9", &effects::Config().HideHud);
        ImGui::MenuItem("Hide hit labels and frame data", nullptr, &effects::Config().HideHitLabels);
        ImGui::MenuItem("Follow the playhead", nullptr, &g_timeline.Follow);
        ImGui::MenuItem("Rule of thirds", nullptr, &camera::Config().RuleOfThirds);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Keys")) {
        // While the editor is open it has the keyboard. Closed, every key goes to the game except Insert.
        static const char* const keys[][2] = {
            { "Insert", "Open or close the editor. Closed, the game has the keyboard" },
            { "Space", "Play and pause" },
            { "Left  Right", "Back and forward 1 s (Shift 5 s, Ctrl 1 frame)" },
            { ",  .", "Back and forward 1 frame" },
            { "Up  Down", "Faster and slower" },
            { "Home", "The very start of the replay" },
            { "Ctrl+Z  Ctrl+Y", "Undo and redo" },
            { "F4", "Free camera looks with the mouse or the arrow keys" },
            { "F5  F6", "Free camera, dolly path" },
            { "F7", "Record" },
            { "F9", "Hide the HUD" },
            { "K", "Add a camera key (free camera)" },
            { "WASD  Q E", "Fly; hold right mouse over the picture to look" },
        };
        for (const auto& key : keys) {
            ImGui::TextUnformatted(key[0]);
            ImGui::SameLine(110);
            ImGui::TextColored(Dim, "%s", key[1]);
        }
        ImGui::TextColored(Dim, "With the free camera on the arrow keys (F4), the arrows look around.");
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Tools")) {
        if (ImGui::MenuItem("Dump game classes"))
            effects::RequestDump();
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("About")) {
        ImGui::TextUnformatted("tekken8mvm: cinematic tools for Tekken 8, offline only.");
        ImGui::TextUnformatted("Layout after IWXMVM. Engine notes from the Tekken Overlay rebuild.");
        ImGui::EndMenu();
    }
    const char* label = "tekken8mvm v0.1 beta";
    ImGui::SameLine(ImGui::GetIO().DisplaySize.x - ImGui::CalcTextSize(label).x - ImGui::GetStyle().WindowPadding.x * 2);
    ImGui::TextColored(Dim, "%s", label);
    ImGui::EndMainMenuBar();

    if (wantReset)
        ImGui::OpenPopup("Reset the mod?");
    if (ImGui::BeginPopupModal("Reset the mod?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("Every setting goes back to how it starts, and the camera path and speed ramp are cleared.");
        ImGui::TextColored(Dim, "Saved presets and shots, your recordings and your pak mods stay. Ctrl+Z brings it back.");
        if (ImGui::Button("Reset")) {
            presets::ResetAll();
            Notice("Reset");
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel"))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

void RightPanel(ImVec2 position, ImVec2 size)
{
    ImGui::SetNextWindowPos(position);
    ImGui::SetNextWindowSize(size);
    ImGui::Begin("##panel", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus);

    // The tabs: an icon over a name, and a line under the one that's open.
    struct TabButton {
        const char* Icon;
        const char* Label;
    };
    static const TabButton tabs[TabCount] = { { ICON_FA_FILM, "Replays" }, { ICON_FA_VIDEO, "Camera" }, { ICON_FA_SLIDERS, "Look" }, { ICON_FA_PERSON, "Scene" }, { ICON_FA_CIRCLE, "Record" } };
    const ImGuiStyle& style = ImGui::GetStyle();
    const float line = ImGui::GetTextLineHeight();
    const float tabWidth = std::floor((ImGui::GetContentRegionAvail().x - style.ItemSpacing.x * (TabCount - 1)) / TabCount);
    const float tabHeight = std::floor(line * 2 + style.FramePadding.y * 3);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    for (int i = 0; i < TabCount; ++i) {
        if (i > 0)
            ImGui::SameLine();
        const bool selected = g_tab == i;
        ImGui::PushID(i);
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(selected ? ImGuiCol_Header : ImGuiCol_Button));
        if (ImGui::Button("##tab", { tabWidth, tabHeight }))
            g_tab = i;
        ImGui::PopStyleColor();
        ImGui::PopID();
        const ImVec2 min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
        g_layout.Tabs[i] = { (min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f };
        const ImU32 colour = ImGui::GetColorU32(selected ? ImGuiCol_Text : ImGuiCol_TextDisabled);
        const ImVec2 iconSize = ImGui::CalcTextSize(tabs[i].Icon), labelSize = ImGui::CalcTextSize(tabs[i].Label);
        draw->AddText({ std::floor((min.x + max.x - iconSize.x) * 0.5f), std::floor(min.y + style.FramePadding.y) }, colour, tabs[i].Icon);
        draw->AddText({ std::floor((min.x + max.x - labelSize.x) * 0.5f), std::floor(max.y - style.FramePadding.y - line) }, colour, tabs[i].Label);
        if (selected)
            draw->AddRectFilled({ min.x, max.y - 2 }, max, ImGui::GetColorU32(ImGuiCol_CheckMark));
    }

    if (ImGui::BeginChild("tabbody", ImVec2(0, 0), ImGuiChildFlags_None)) {
        switch (g_tab) {
        case 0:
            ReplaysTab();
            break;
        case 1:
            CameraTab();
            break;
        case 2:
            VisualsTab();
            break;
        case 3:
            SceneTab();
            break;
        default:
            RecordTab();
            break;
        }
    }
    ImGui::EndChild();
    ImGui::End();
}

// The strip over the picture: which camera the picture is from.
void ViewBar(ImVec2 position, ImVec2 size)
{
    ImGui::SetNextWindowPos(position);
    ImGui::SetNextWindowSize(size);
    ImGui::Begin("##viewbar", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus);
    camera::Settings& s = camera::Config();
    static const char* const names[] = { "Game Camera", "Free Camera", "Dolly Camera", "Attached Camera" };
    const int mode = std::clamp(static_cast<int>(s.Mode), 0, 3);
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12);
    if (ImGui::BeginCombo("##viewcamera", names[mode])) {
        for (int i = 0; i < 4; ++i)
            if (ImGui::Selectable(names[i], i == mode) && i != mode) {
                if (i == 1)
                    camera::ToggleFree();
                else if (i == 2)
                    camera::ToggleDolly();
                else
                    s.Mode = i == 3 ? camera::Mode::Attach : camera::Mode::Game;
            }
        ImGui::EndCombo();
    }
    if (g_notice[0]) {
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(Dim, "%s", g_notice);
    }
    ImGui::End();
}

} // namespace

void SetTab(int tab)
{
    g_tab = tab;
}

void OpenAllGroups(bool open)
{
    g_openAllGroups = open;
}

// Drives the real editor with mouse events, one action per frame, and logs what happened. Harness only.
bool RunUiTest()
{
    static std::vector<std::function<void()>> steps;
    static size_t at = 0;
    static ImVec2 pointer { 5, 5 };
    ImGuiIO& io = ImGui::GetIO();

    if (steps.empty()) {
        g_openAllGroups = true;
        const auto wait = [&](int frames) {
            for (int i = 0; i < frames; ++i)
                steps.push_back([] {});
        };
        const auto moveTo = [&](std::function<ImVec2()> where) {
            steps.push_back([where] {
                pointer = where();
            });
            wait(1);
        };
        const auto button = [&](int which, bool down) {
            steps.push_back([&io, which, down] { io.AddMouseButtonEvent(which, down); });
            wait(1);
        };
        const auto click = [&](std::function<ImVec2()> where, int which = 0) {
            moveTo(where);
            button(which, true);
            button(which, false);
            wait(1);
        };
        const auto drag = [&](std::function<ImVec2()> from, std::function<ImVec2()> to, int which = 0) {
            moveTo(from);
            button(which, true);
            moveTo(to);
            moveTo(to);
            button(which, false);
            wait(1);
        };
        const auto check = [&](const char* name, std::function<bool()> test) {
            steps.push_back([name, test] { mod::Log("[uitest] %s  %s", test() ? "PASS" : "FAIL", name); });
        };
        const auto keyAt = [](int index) -> ImVec2 {
            const auto keys = camera::Keys();
            const float mid = (g_layout.CameraTop + g_layout.LookTop) * 0.5f;
            return { TimeToX(keys[std::min<size_t>(index, keys.size() - 1)].Time, g_layout.Left, g_layout.Width), mid };
        };
        const auto speedAt = [](int index) -> ImVec2 {
            const auto keys = game::SpeedKeys();
            const auto& key = keys[std::min<size_t>(index, keys.size() - 1)];
            return { TimeToX(key.Time, g_layout.Left, g_layout.Width), SpeedToY(key.Speed, g_layout.SpeedTop, g_layout.RangeTop - g_layout.SpeedTop) };
        };

        wait(10);

        // Tabs
        for (int i = 0; i < TabCount; ++i) {
            click([i] { return g_layout.Tabs[i]; });
            wait(4); // every control on the tab gets drawn with its sections open
            check("tab button switches tab", [i] { return g_tab == i; });
        }

        // The camera only takes the mouse over the picture.
        moveTo([] { return g_layout.ViewCenter; });
        wait(1);
        check("mouse over the picture counts as the game view", [] { return overlay::MouseOverGameView(); });
        moveTo([] { return g_layout.PanelCenter; });
        wait(1);
        check("mouse over the panel doesn't", [] { return !overlay::MouseOverGameView(); });

        // Camera keys
        static double before = 0;
        static size_t countBefore = 0;
        steps.push_back([] {
            const auto keys = camera::Keys();
            before = keys[1].Time;
            countBefore = keys.size();
        });
        drag([keyAt] { return keyAt(1); }, [keyAt] { return ImVec2(TimeToX(3.6, g_layout.Left, g_layout.Width), (g_layout.CameraTop + g_layout.LookTop) * 0.5f); });
        check("dragging a camera key moves it", [] {
            const auto keys = camera::Keys();
            return keys.size() == countBefore && keys[1].Time > before + 0.3 && std::fabs(keys[1].Time - 3.6) < 0.1;
        });
        check("camera keys stay in time order", [] {
            const auto keys = camera::Keys();
            return std::is_sorted(keys.begin(), keys.end(), [](const cine::CameraKey& a, const cine::CameraKey& b) { return a.Time < b.Time; });
        });
        drag([keyAt] { return keyAt(1); }, [] { return ImVec2(TimeToX(6.0, g_layout.Left, g_layout.Width), (g_layout.CameraTop + g_layout.LookTop) * 0.5f); });
        check("dragging a key past its neighbour re-sorts", [] {
            const auto keys = camera::Keys();
            return keys.size() == countBefore && std::is_sorted(keys.begin(), keys.end(), [](const cine::CameraKey& a, const cine::CameraKey& b) { return a.Time < b.Time; })
                && std::fabs(keys[2].Time - 6.0) < 0.1;
        });
        click([keyAt] { return keyAt(0); }, 1);
        check("right-clicking a camera key deletes it", [] { return camera::Keys().size() == countBefore - 1; });

        // Speed ramp
        static size_t speedBefore = 0;
        steps.push_back([] {
            game::Time().UseSpeedRamp = false;
            speedBefore = game::SpeedKeys().size();
        });
        const auto emptySpot = [] { return ImVec2(TimeToX(15.0, g_layout.Left, g_layout.Width), (g_layout.SpeedTop + g_layout.RangeTop) * 0.5f); };
        click(emptySpot);
        check("clicking the curve adds a speed key", [] { return game::SpeedKeys().size() == speedBefore + 1; });
        check("adding a speed key turns the ramp on", [] { return game::Time().UseSpeedRamp; });
        static double speedValue = 0;
        steps.push_back([] { speedValue = game::SpeedKeys().back().Speed; });
        drag([speedAt] { return speedAt(99); }, [] { return ImVec2(TimeToX(18.0, g_layout.Left, g_layout.Width), g_layout.RangeTop - 2); });
        check("dragging a speed key changes its time and speed", [] {
            const auto keys = game::SpeedKeys();
            return keys.size() == speedBefore + 1 && std::fabs(keys.back().Time - 18.0) < 0.2 && keys.back().Speed < speedValue;
        });
        check("speed keys never go below 0.01", [] { return game::SpeedKeys().back().Speed >= MinSpeed - 1e-9; });
        click([speedAt] { return speedAt(99); }, 1);
        check("right-clicking a speed key deletes it", [] { return game::SpeedKeys().size() == speedBefore; });

        // Range
        static double rangeOut = 0, rangeIn = 0;
        steps.push_back([] {
            rangeOut = recorder::Config().RangeOut;
            rangeIn = recorder::Config().RangeIn;
        });
        drag([] { return ImVec2(TimeToX(recorder::Config().RangeOut, g_layout.Left, g_layout.Width), (g_layout.RangeTop + g_layout.Bottom) * 0.5f); },
             [] { return ImVec2(TimeToX(20.0, g_layout.Left, g_layout.Width), (g_layout.RangeTop + g_layout.Bottom) * 0.5f); });
        check("dragging the range end moves it", [] { return std::fabs(recorder::Config().RangeOut - 20.0) < 0.2; });
        drag([] { return ImVec2(TimeToX(recorder::Config().RangeIn, g_layout.Left, g_layout.Width), (g_layout.RangeTop + g_layout.Bottom) * 0.5f); },
             [] { return ImVec2(TimeToX(5.0, g_layout.Left, g_layout.Width), (g_layout.RangeTop + g_layout.Bottom) * 0.5f); });
        check("dragging the range start moves it", [] { return std::fabs(recorder::Config().RangeIn - 5.0) < 0.2; });
        drag([] { return ImVec2(TimeToX(5.0, g_layout.Left, g_layout.Width), (g_layout.RangeTop + g_layout.Bottom) * 0.5f); },
             [] { return ImVec2(TimeToX(25.0, g_layout.Left, g_layout.Width), (g_layout.RangeTop + g_layout.Bottom) * 0.5f); });
        check("the range start can't pass the end", [] { return recorder::Config().RangeIn < recorder::Config().RangeOut; });

        // Ruler
        click([] { return ImVec2(TimeToX(12.0, g_layout.Left, g_layout.Width), g_layout.RulerTop + 4); });
        check("clicking ahead on the ruler runs the fight there", [] { return game::Seeking(); });
        steps.push_back([] { game::SeekTo(-1.0); });

        // Playback bar
        click([] { return g_layout.PlayButton; });
        check("the pause button pauses", [] { return game::Time().Paused; });
        click([] { return g_layout.PlayButton; });
        check("the pause button resumes", [] { return !game::Time().Paused; });
        steps.push_back([] { game::Time().UseSpeedRamp = false; });
        wait(1);
        drag([] { return ImVec2((g_layout.SliderMin.x + g_layout.SliderMax.x) * 0.5f, (g_layout.SliderMin.y + g_layout.SliderMax.y) * 0.5f); },
             [] { return ImVec2(g_layout.SliderMin.x - 50, (g_layout.SliderMin.y + g_layout.SliderMax.y) * 0.5f); });
        check("the timescale slider reaches 0.01", [] { return std::fabs(game::Time().Speed - 0.01) < 0.0005; });
        click([] { return g_layout.OneX; });
        check("the 1x button goes back to normal speed", [] { return game::Time().Speed == 1.0; });

        // Zoom and pan
        static double span = 0;
        steps.push_back([] { span = g_timeline.End - g_timeline.Start; });
        moveTo([] { return ImVec2(g_layout.Left + g_layout.Width * 0.5f, (g_layout.CameraTop + g_layout.LookTop) * 0.5f); });
        steps.push_back([&io] { io.AddMouseWheelEvent(0, 3); });
        wait(2);
        check("the wheel zooms the timeline", [] { return g_timeline.End - g_timeline.Start < span * 0.8; });
        steps.push_back([] { span = g_timeline.Start; });
        drag([] { return ImVec2(g_layout.Left + g_layout.Width * 0.5f, (g_layout.CameraTop + g_layout.LookTop) * 0.5f); },
             [] { return ImVec2(g_layout.Left + g_layout.Width * 0.3f, (g_layout.CameraTop + g_layout.LookTop) * 0.5f); }, 2);
        check("middle-dragging pans the timeline", [] { return g_timeline.Start != span && !g_timeline.Follow; });
        steps.push_back([] { g_timeline.Follow = true; });

        // Saving and loading keep everything.
        static size_t savedKeys = 0;
        static size_t savedSpeed = 0;
        check("a shot saves", [] {
            game::Time().Speed = 0.25;
            effects::Config().NoBloom = true;
            postfx::All()[0].On = true;
            postfx::All()[0].Value = 1.7f;
            lights::Rig()[1].On = true;
            lights::Rig()[1].Intensity = 1234;
            recorder::Config().MotionSamples = 16;
            savedKeys = camera::Keys().size();
            savedSpeed = game::SpeedKeys().size();
            return presets::SaveShot("uitest");
        });
        check("a preset saves", [] { return presets::SavePreset("uitest"); });
        check("a shot loads back the same", [] {
            game::Time().Speed = 1.0;
            effects::Config().NoBloom = false;
            postfx::All()[0].On = false;
            lights::Rig()[1].On = false;
            recorder::Config().MotionSamples = 1;
            camera::ClearKeys();
            game::SetSpeedKeys({});
            return presets::LoadShot("uitest") && camera::Keys().size() == savedKeys && game::SpeedKeys().size() == savedSpeed && game::Time().Speed == 0.25
                && effects::Config().NoBloom && postfx::All()[0].On && std::fabs(postfx::All()[0].Value - 1.7f) < 0.001f && lights::Rig()[1].On
                && lights::Rig()[1].Intensity == 1234 && recorder::Config().MotionSamples == 16;
        });
        check("a preset loads without touching the camera path", [] {
            camera::ClearKeys();
            return presets::LoadPreset("uitest") && camera::Keys().empty() && game::Time().Speed == 0.25;
        });
        check("a path saves and loads", [] {
            camera::SetKeys({ { 1, { 1, 2, 3 }, { 4, 5, 6 }, 40 }, { 2, { 7, 8, 9 }, { 1, 1, 1 }, 50 } });
            const bool saved = camera::SavePath("uitest");
            camera::ClearKeys();
            return saved && camera::LoadPath("uitest") && camera::Keys().size() == 2 && camera::Keys()[1].Location.Y == 8 && camera::Keys()[1].Fov == 50;
        });
        check("a CamIO file writes", [] { return camera::ExportCamIo("uitest", 60); });
        check("list of saved shots has it", [] {
            const auto shots = presets::SavedShots();
            return std::find(shots.begin(), shots.end(), "uitest") != shots.end();
        });
        // With the stand-in for ReShade loaded (the harness's -ReShade): the link finds it, lists it, sets it and keys it.
        if (GetEnvironmentVariableW(L"TEKKEN8MVM_HARNESS_RESHADE", nullptr, 0) > 0) {
            static HMODULE fake = nullptr;
            static float (*fakeValue)(int, int) = nullptr;
            static int (*fakeTechnique)(int) = nullptr;
            static int (*fakeCount)(int) = nullptr;
            const auto value = [](const std::vector<reshadelink::Value>& values, const char* effect, const char* name) -> const reshadelink::Value* {
                for (const reshadelink::Value& v : values)
                    if (v.Effect == effect && v.Name == name)
                        return &v;
                return nullptr;
            };
            // The link looks for ReShade every couple of seconds of game ticks.
            for (int i = 0; i < 40; ++i) {
                steps.push_back([] {});
                wait(10);
            }
            check("ReShade is found and takes the add-on", [] {
                fake = GetModuleHandleW(L"tekken8mvm_fake_reshade.dll");
                fakeValue = fake ? reinterpret_cast<float (*)(int, int)>(GetProcAddress(fake, "FakeReShadeValue")) : nullptr;
                fakeTechnique = fake ? reinterpret_cast<int (*)(int)>(GetProcAddress(fake, "FakeReShadeTechnique")) : nullptr;
                fakeCount = fake ? reinterpret_cast<int (*)(int)>(GetProcAddress(fake, "FakeReShadeCount")) : nullptr;
                return fakeValue && fakeTechnique && fakeCount && reshadelink::Found();
            });
            check("an older ReShade is asked again with an API version it takes", [] { return fakeCount && fakeCount(1) == 18 && fakeCount(2) == 2; });
            wait(40);
            check("with ReShade drawing, the overlay waits for its effects", [] { return reshadelink::OwnsPresent() && fakeCount && fakeCount(0) > 0; });
            check("ReShade's techniques are listed", [] {
                const auto techniques = reshadelink::Techniques();
                return techniques.size() == 2 && techniques[0].Name == "Bloom" && techniques[0].Effect == "Bloom.fx" && techniques[0].On && !techniques[1].On;
            });
            check("only the settings of effects that are on are listed", [value] {
                const auto values = reshadelink::Values();
                const reshadelink::Value* strength = value(values, "Bloom.fx", "Strength");
                const reshadelink::Value* colour = value(values, "Bloom.fx", "Colour");
                const reshadelink::Value* passes = value(values, "Bloom.fx", "Passes");
                const reshadelink::Value* soft = value(values, "Bloom.fx", "Soft");
                return values.size() == 4 && strength && strength->Label == "Bloom strength" && std::fabs(strength->Numbers[0] - 0.5f) < 1e-6f && strength->Max == 2.0f && colour
                    && colour->Count == 3 && colour->Colour && passes && passes->Kind == reshadelink::Kind::Int && passes->Numbers[0] == 3 && passes->Min == 1 && soft
                    && soft->Kind == reshadelink::Kind::Bool && !value(values, "Bloom.fx", "Timer") && !value(values, "Tint.fx", "Amount");
            });
            steps.push_back([] {
                const float strength[4] = { 1.25f }, passes[4] = { 5.4f }, colour[4] = { 0.1f, 0.2f, 0.3f };
                reshadelink::SetValue("Bloom.fx", "Strength", strength);
                reshadelink::SetValue("Bloom.fx", "Passes", passes);
                reshadelink::SetValue("Bloom.fx", "Colour", colour);
                reshadelink::SetTechnique("Tint.fx", "Tint", true);
            });
            wait(40);
            check("setting a value reaches ReShade", [] {
                return std::fabs(fakeValue(0, 0) - 1.25f) < 1e-6f && fakeValue(2, 0) == 5.0f && std::fabs(fakeValue(1, 1) - 0.2f) < 1e-6f && std::fabs(fakeValue(1, 2) - 0.3f) < 1e-6f;
            });
            check("switching a technique on reaches ReShade and lists its settings", [value] {
                const auto values = reshadelink::Values();
                return fakeTechnique(1) == 1 && values.size() == 5 && value(values, "Tint.fx", "Amount");
            });
            check("a look holds ReShade's techniques and values", [] {
                Ini look;
                reshadelink::WriteLook(look);
                return look.Get("reshade.t.Bloom.fx.Bloom") == "1" && look.Get("reshade.t.Tint.fx.Tint") == "1" && std::fabs(look.GetNumber("reshade.u.Bloom.fx.Strength.0", 0) - 1.25) < 1e-6
                    && std::fabs(look.GetNumber("reshade.u.Bloom.fx.Colour.2", 0) - 0.3) < 1e-6 && look.GetNumber("reshade.u.Bloom.fx.Passes.0", 0) == 5;
            });
            check("which ReShade entries move smoothly between look keys", [] {
                return reshadelink::MovesSmoothly("reshade.u.Bloom.fx.Strength.0") && reshadelink::MovesSmoothly("reshade.u.Bloom.fx.Passes.0") && !reshadelink::MovesSmoothly("reshade.u.Bloom.fx.Soft.0")
                    && !reshadelink::MovesSmoothly("reshade.t.Bloom.fx.Bloom");
            });
            steps.push_back([] {
                // What a look key between two others would ask for.
                Ini look;
                look.Set("reshade.u.Bloom.fx.Strength.0", "0.75");
                look.SetBool("reshade.t.Bloom.fx.Bloom", false);
                reshadelink::ReadLook(look);
            });
            wait(40);
            check("a look key sets ReShade, and leaves what it doesn't mention", [] {
                return std::fabs(fakeValue(0, 0) - 0.75f) < 1e-6f && fakeTechnique(0) == 0 && fakeTechnique(1) == 1 && fakeValue(2, 0) == 5.0f;
            });
            // The ReShade section of the Look tab, drawn with real entries in it.
            click([] { return g_layout.Tabs[2]; });
            wait(6);
            check("the editor still draws with ReShade's settings in it", [] { return g_tab == 2; });
        }
        steps.push_back([] { mod::Log("[uitest] finished"); });
    }

    io.AddMousePosEvent(pointer.x, pointer.y);
    if (at < steps.size())
        steps[at++]();
    return at >= steps.size();
}

// Iris Studio starts from ImGui's own style and sets the padding from the font's size. The colours are its palette, mapped as its
// UI/Theme.cpp maps them.
void ApplyStyle(float fontSize)
{
    ImGuiStyle& style = ImGui::GetStyle();
    style = ImGuiStyle();
    const float scale = fontSize / 15.0f; // ImGui's sizes suit a font of about that
    style.ScaleAllSizes(scale);
    style.FramePadding = { std::floor(fontSize * 0.28f), std::floor(fontSize * 0.28f) };
    style.WindowPadding = { std::floor(fontSize * 0.55f), std::floor(fontSize * 0.45f) };
    style.WindowBorderSize = 0;
    style.WindowRounding = 0;

    const uint32_t hover = Mix(pal::Raised, pal::Text, 0.08f);         // a step up from raised, for what's under the pointer
    const uint32_t selected = Mix(pal::Raised, pal::Accent, 0.30f);    // the accent mixed in for what's chosen
    const uint32_t held = Mix(pal::Raised, pal::Accent, 0.40f);        // and for what's held down
    const uint32_t accentHover = Mix(pal::Accent, 0xFFFFFF, 0.12f);
    ImVec4* c = style.Colors;
    c[ImGuiCol_Text] = Colour(pal::Text);
    c[ImGuiCol_TextDisabled] = Colour(pal::Muted);
    c[ImGuiCol_WindowBg] = Colour(pal::Bg);
    c[ImGuiCol_ChildBg] = Colour(pal::Bg, 0.0f);
    c[ImGuiCol_PopupBg] = Colour(pal::Surface, 0.98f);
    c[ImGuiCol_Border] = Colour(pal::Border);
    c[ImGuiCol_BorderShadow] = Colour(pal::Bg, 0.0f);
    c[ImGuiCol_FrameBg] = Colour(pal::Surface);
    c[ImGuiCol_FrameBgHovered] = Colour(Mix(pal::Surface, pal::Text, 0.07f));
    c[ImGuiCol_FrameBgActive] = Colour(Mix(pal::Surface, pal::Text, 0.12f));
    c[ImGuiCol_TitleBg] = Colour(pal::Surface);
    c[ImGuiCol_TitleBgActive] = Colour(pal::Raised);
    c[ImGuiCol_TitleBgCollapsed] = Colour(pal::Surface);
    c[ImGuiCol_MenuBarBg] = Colour(pal::Surface);
    c[ImGuiCol_ScrollbarBg] = Colour(pal::Bg, 0.6f);
    c[ImGuiCol_ScrollbarGrab] = Colour(pal::Raised);
    c[ImGuiCol_ScrollbarGrabHovered] = Colour(hover);
    c[ImGuiCol_ScrollbarGrabActive] = Colour(held);
    c[ImGuiCol_CheckMark] = Colour(pal::Accent);
    c[ImGuiCol_SliderGrab] = Colour(pal::Accent);
    c[ImGuiCol_SliderGrabActive] = Colour(accentHover);
    c[ImGuiCol_Button] = Colour(pal::Raised);
    c[ImGuiCol_ButtonHovered] = Colour(hover);
    c[ImGuiCol_ButtonActive] = Colour(held);
    c[ImGuiCol_Header] = Colour(selected);
    c[ImGuiCol_HeaderHovered] = Colour(hover);
    c[ImGuiCol_HeaderActive] = Colour(held);
    c[ImGuiCol_Separator] = Colour(pal::Border);
    c[ImGuiCol_SeparatorHovered] = Colour(pal::Muted);
    c[ImGuiCol_SeparatorActive] = Colour(pal::Accent);
    c[ImGuiCol_ResizeGrip] = Colour(pal::Bg, 0.0f);
    c[ImGuiCol_ResizeGripHovered] = Colour(pal::Muted, 0.6f);
    c[ImGuiCol_ResizeGripActive] = Colour(pal::Accent, 0.8f);
    c[ImGuiCol_Tab] = Colour(pal::Surface);
    c[ImGuiCol_TabHovered] = Colour(hover);
    c[ImGuiCol_TabSelected] = Colour(selected);
    c[ImGuiCol_PlotLines] = Colour(pal::Muted);
    c[ImGuiCol_PlotLinesHovered] = Colour(pal::Accent);
    c[ImGuiCol_PlotHistogram] = Colour(pal::Raised);
    c[ImGuiCol_PlotHistogramHovered] = Colour(hover);
    c[ImGuiCol_TableHeaderBg] = Colour(pal::Raised);
    c[ImGuiCol_TableBorderStrong] = Colour(pal::Border);
    c[ImGuiCol_TableBorderLight] = Colour(Mix(pal::Border, pal::Bg, 0.4f));
    c[ImGuiCol_TableRowBg] = Colour(pal::Bg, 0.0f);
    c[ImGuiCol_TableRowBgAlt] = Colour(pal::Text, 0.03f);
    c[ImGuiCol_TextSelectedBg] = Colour(pal::Accent, 0.30f);
    c[ImGuiCol_DragDropTarget] = Colour(pal::Accent);
    c[ImGuiCol_ModalWindowDimBg] = Colour(0x000000, 0.5f);
}

void Draw()
{
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    ImDrawList* background = ImGui::GetBackgroundDrawList();
    background->AddRectFilled({ 0, 0 }, display, Hex(pal::Bg));

    MenuBar();
    const float font = ImGui::GetFontSize();
    const float menuHeight = ImGui::GetFrameHeight();
    const float stripHeight = ImGui::GetFrameHeight() + ImGui::GetStyle().WindowPadding.y * 2;
    const float barHeight = ImGui::GetFrameHeight() + ImGui::GetStyle().WindowPadding.y * 2;

    // The picture keeps its shape at the top left, under a strip that says which camera it's from. The panel is to its right, and the
    // playback bar and the timeline run under both. The timeline always gets room for its lanes.
    const float aspect = overlay::GameAspect() > 0 ? overlay::GameAspect() : display.x / display.y;
    const float viewWidth = std::floor(display.x * 0.70f);
    const float most = std::max(display.y - menuHeight - stripHeight - barHeight - font * 11.0f, font * 6.0f);
    const float viewHeight = std::floor(std::min(viewWidth / aspect, most));
    const float pictureWidth = std::floor(std::min(viewWidth, viewHeight * aspect));
    const float viewTop = menuHeight + stripHeight;
    const ImVec2 viewMin { std::floor((viewWidth - pictureWidth) * 0.5f), viewTop }, viewMax { viewMin.x + pictureWidth, viewTop + viewHeight };

    if (const uint64_t texture = overlay::GameTexture())
        background->AddImage(static_cast<ImTextureID>(texture), viewMin, viewMax);

    const ImVec2 mouse = ImGui::GetMousePos();
    overlay::SetGameViewHovered(mouse.x >= viewMin.x && mouse.x < viewMax.x && mouse.y >= viewMin.y && mouse.y < viewMax.y && !ImGui::IsAnyItemActive()
                                && !ImGui::GetIO().WantTextInput);

    // Along the bottom of the picture: what the view is doing right now, and the keys for it.
    const char* hint = nullptr;
    switch (camera::Config().Mode) {
    case camera::Mode::Free:
        hint = "hold right mouse  look      W A S D  fly      K  add a key      Space  play";
        break;
    case camera::Mode::Dolly:
        hint = "playing the camera path";
        break;
    case camera::Mode::Attach:
        hint = "right mouse and wheel  orbit";
        break;
    default:
        break;
    }
    {
        const float band = ImGui::GetFrameHeight();
        background->AddRectFilled({ viewMin.x, viewMax.y - band }, viewMax, IM_COL32(0, 0, 0, 110));
        const float y = viewMax.y - band + (band - font) * 0.5f;
        if (hint)
            background->AddText({ viewMin.x + font * 0.6f, y }, Hex(pal::Text), hint);
        const char* shortcuts = "?  all shortcuts: the Keys menu";
        background->AddText({ viewMax.x - ImGui::CalcTextSize(shortcuts).x - font * 0.6f, y }, Hex(pal::Muted), shortcuts);
    }

    if (camera::Config().RuleOfThirds)
        DrawThirds(background, viewMin, viewMax);
    presets::UndoWatch(!ImGui::IsAnyItemActive() && !ImGui::IsMouseDown(ImGuiMouseButton_Left));

    g_layout.ViewCenter = { (viewMin.x + viewMax.x) * 0.5f, (viewMin.y + viewMax.y) * 0.5f };
    g_layout.PanelCenter = { (viewWidth + display.x) * 0.5f, viewTop + viewHeight * 0.5f };

    ViewBar({ 0, menuHeight }, { viewWidth, stripHeight });
    RightPanel({ viewWidth, menuHeight }, { display.x - viewWidth, stripHeight + viewHeight });
    ControlBar({ 0, viewMax.y }, { display.x, barHeight });
    const float timelineTop = viewMax.y + barHeight;
    Timeline({ 0, timelineTop }, { display.x, std::max(display.y - timelineTop, 1.0f) });

    // Thin lines between the parts.
    ImDrawList* lines = ImGui::GetForegroundDrawList();
    lines->AddLine({ viewWidth, menuHeight }, { viewWidth, viewMax.y }, Hex(pal::Border));
    lines->AddLine({ 0, viewMax.y }, { display.x, viewMax.y }, Hex(pal::Border));
    lines->AddLine({ 0, menuHeight }, { display.x, menuHeight }, Hex(pal::Border));
}

void ShowNotice(const char* text)
{
    Notice(text);
}

std::mutex g_flashMutex;
std::string g_flash;
ULONGLONG g_flashUntil = 0;

void Flash(const std::string& text)
{
    std::lock_guard lock(g_flashMutex);
    g_flash = text;
    g_flashUntil = GetTickCount64() + 5000;
    Notice(text.c_str());
}

// The flash message, low in the middle of the picture, on a dark band so it reads over anything.
void DrawFlash()
{
    std::string text;
    {
        std::lock_guard lock(g_flashMutex);
        if (GetTickCount64() >= g_flashUntil)
            return;
        text = RightToLeftFixed(g_flash);
    }
    overlay::AddFontCharacters(text);
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const ImVec2 size = ImGui::CalcTextSize(text.c_str());
    const ImVec2 at(std::floor((display.x - size.x) * 0.5f), std::floor(display.y * 0.86f));
    ImDrawList* draw = ImGui::GetForegroundDrawList();
    draw->AddRectFilled({ at.x - 14, at.y - 8 }, { at.x + size.x + 14, at.y + size.y + 8 }, IM_COL32(0, 0, 0, 190), 6.0f);
    draw->AddText(at, Hex(pal::Text), text.c_str());
}

void DrawOsd()
{
    DrawFlash();
    if (camera::Config().RuleOfThirds)
        DrawThirds(ImGui::GetForegroundDrawList(), { 0, 0 }, ImGui::GetIO().DisplaySize);
    std::string text;
    const game::TimeControl& t = game::Time();
    if (camera::Config().Mode == camera::Mode::Free)
        text += "Free Camera   ";
    else if (camera::Config().Mode == camera::Mode::Dolly)
        text += "Dolly Camera   ";
    else if (camera::Config().Mode == camera::Mode::Attach)
        text += "Attached Camera   ";
    if (t.Paused)
        text += ICON_FA_PAUSE "  paused   ";
    else if (t.Speed != 1.0) {
        char speed[32];
        snprintf(speed, sizeof(speed), "%.3gx   ", t.Speed);
        text += speed;
    }
    std::string recording;
    if (recorder::Active()) {
        char frames[48];
        snprintf(frames, sizeof(frames), ICON_FA_CIRCLE "  REC %llu", static_cast<unsigned long long>(recorder::Frames()));
        recording = frames;
    } else if (recorder::Finishing()) {
        recording = "saving";
    }
    if (text.empty() && recording.empty())
        return;
    ImDrawList* draw = ImGui::GetForegroundDrawList();
    const float width = ImGui::CalcTextSize(text.c_str()).x;
    draw->AddText(ImVec2(13, 13), IM_COL32(0, 0, 0, 200), (text + recording).c_str());
    draw->AddText(ImVec2(12, 12), Hex(pal::Text), text.c_str());
    draw->AddText(ImVec2(12 + width, 12), Hex(pal::Bad), recording.c_str());
}

} // namespace menu
