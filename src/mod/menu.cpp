// The editor (Insert), laid out like IWXMVM: a menu bar on top, the game picture scaled down on the left, tool tabs on
// the right, and across the bottom the playback controls and the keyframe timeline.
#include <imgui.h>

#include <windows.h>

#include <shellapi.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "mod/audio.h"
#include "mod/camera.h"
#include "mod/depth.h"
#include "mod/dvars.h"
#include "mod/effects.h"
#include "mod/game.h"
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
#include "mod/sharing.h"
#include "mod/switcher.h"

namespace menu {

namespace {

const ImVec4 Accent(0.90f, 0.20f, 0.25f, 1.0f);
const ImVec4 Dim(0.62f, 0.62f, 0.66f, 1.0f);
const ImVec4 Good(0.35f, 0.82f, 0.50f, 1.0f);
const ImVec4 Warn(0.95f, 0.72f, 0.30f, 1.0f);
constexpr double MinSpeed = 0.01, MaxSpeed = 4.0;

int g_tab = 0;
char g_notice[160] = "";

void Notice(const char* text)
{
    strncpy_s(g_notice, text, _TRUNCATE);
}

// The UI test opens every section so that all the controls get drawn.
bool g_openAllGroups = false;

bool Group(const char* label)
{
    if (g_openAllGroups)
        ImGui::SetNextItemOpen(true);
    return ImGui::CollapsingHeader(label);
}

// Where things ended up on screen, for the UI test to click on.
struct Layout {
    float Left = 0, Width = 1, RulerTop = 0, CameraTop = 0, SpeedTop = 0, RangeTop = 0, Bottom = 0;
    ImVec2 PlayButton, OneX, SliderMin, SliderMax, ViewCenter, PanelCenter;
    ImVec2 Tabs[4];
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

void Section(const char* title)
{
    ImGui::Spacing();
    ImGui::TextColored(Accent, "%s", title);
    ImGui::Separator();
}

// Sliders in the narrow panel: the bar takes just over half the width, and the label sits beside it.
void BarWidth()
{
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.56f);
}

bool SliderDouble(const char* label, double& value, double min, double max, const char* format = "%.2f", ImGuiSliderFlags flags = 0)
{
    float f = static_cast<float>(value);
    BarWidth();
    const bool changed = ImGui::SliderFloat(label, &f, static_cast<float>(min), static_cast<float>(max), format, flags);
    if (changed)
        value = f;
    return changed;
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
    static char exportName[64] = "replay1";
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
            ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.62f, 0.15f, 0.19f, 1));
            if (ImGui::Selectable(row.c_str(), selected == slot.Index, ImGuiSelectableFlags_AllowDoubleClick)) {
                selected = slot.Index;
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && !switcher::Start(slot.Index, message))
                    mod::Log("Switch: %s", message.c_str());
            }
            ImGui::PopStyleColor();
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
        Help("Leaves this replay and starts the one you picked, through the game's own screens, in about 30 seconds. Don't touch the keyboard until it's done. Double-click a row does the same. It needs the game's list sorted by New.");
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
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.6f);
    ImGui::InputText("##exportname", exportName, sizeof(exportName));
    ImGui::SameLine();
    ImGui::BeginDisabled(selected < 0);
    if (ImGui::Button("Export")) {
        std::string error;
        message = sharing::Export(selected, exportName, error) ? std::string("Saved ") + exportName + ".t8replay" : error;
        files = sharing::SharedFiles();
    }
    ImGui::EndDisabled();
    Help("Pick a replay in the list, name the file, and send it to a friend. It holds both players' names. It only plays on the same game version.");
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
    BarWidth();
    if (ImGui::Combo("Style", &style, "Fixed to bone\0Orbit around bone\0Watch bone\0Fighter POV\0"))
        a.Style = static_cast<camera::AttachStyle>(style);

    if (a.Style == camera::AttachStyle::Pov) {
        ImGui::Checkbox("Hide their head", &a.PovHideHead);
        Help("Their own head and hair would fill the picture. Off shows them, for a mask or a helmet you want in view.");
        ImGui::SameLine();
        ImGui::Checkbox("Keep the horizon level", &a.PovLevel);
        BarWidth();
        ImGui::SliderFloat3("Eye position", a.PovOffset, -40.0f, 40.0f, "%.0f");
        Help("Forward, right and up from the head bone, in centimetres. Fighters are different sizes, so nudge this if the view starts inside a face.");
        BarWidth();
        ImGui::SliderFloat3("Look", a.PovTurn, -180.0f, 180.0f, "%.0f");
        BarWidth();
        ImGui::SliderFloat("POV field of view", &a.PovFov, 40.0f, 130.0f, "%.0f");
        BarWidth();
        ImGui::SliderFloat("Smoothing", &a.Smooth, 0.0f, 0.98f, "%.2f");
        return;
    }

    const std::vector<std::string> bones = camera::Bones(a.Fighter);
    BarWidth();
    if (ImGui::BeginCombo("Bone", a.Bone[0] ? a.Bone : "(whole fighter)", ImGuiComboFlags_HeightLarge)) {
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
        BarWidth();
        ImGui::SliderFloat3("Offset", a.Offset, -300.0f, 300.0f, "%.0f");
        BarWidth();
        ImGui::SliderFloat3("Turn", a.Turn, -180.0f, 180.0f, "%.0f");
    } else if (a.Style == camera::AttachStyle::Orbit) {
        BarWidth();
        ImGui::SliderFloat("Distance", &a.OrbitDistance, 10.0f, 3000.0f, "%.0f cm", ImGuiSliderFlags_Logarithmic);
        ImGui::TextColored(Dim, "Right mouse turns, wheel sets the distance.");
    }
    BarWidth();
    ImGui::SliderFloat("Smoothing", &a.Smooth, 0.0f, 0.98f, "%.2f");
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
    BarWidth();
    if (ImGui::Combo("Dolly style", &pathStyle, "Smooth\0Linear\0CoDMVM\0"))
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

// ----- VISUALS tab

void TimeGroup()
{
    game::TimeControl& t = game::Time();
    float speed = static_cast<float>(t.Speed);
    BarWidth();
    ImGui::BeginDisabled(t.UseSpeedRamp);
    if (ImGui::SliderFloat("Timescale", &speed, static_cast<float>(MinSpeed), static_cast<float>(MaxSpeed), "%.3f", ImGuiSliderFlags_Logarithmic))
        t.Speed = speed;
    ImGui::EndDisabled();
    ImGui::Checkbox("Speed ramp from the timeline", &t.UseSpeedRamp);
    ImGui::Checkbox("Blend motion between game steps", &t.Interpolate);
    Help("The game only moves in 60 Hz steps. Blending poses between steps is what makes slow motion smooth.");
    ImGui::Checkbox("No slow motion on big hits and KOs", &t.NoGameSlowMotion);
    Help("The game slows the world for a moment on some hits, and almost stops it at a KO. This keeps it at the timescale you set.");
    ImGui::Checkbox("Slow the effects too", &t.ScaleEffects);
    Help("Sparks, cloth and materials follow the timescale through the engine's time dilation.");
    float cap = static_cast<float>(t.FpsCap);
    BarWidth();
    if (ImGui::SliderFloat("FPS cap", &cap, 0.0f, 360.0f, cap <= 0 ? "game default" : "%.0f"))
        t.FpsCap = cap;
}

void DvarGroup(const char* group)
{
    for (dvars::Slider& slider : dvars::All()) {
        if (slider.Group != group)
            continue;
        ImGui::PushID(slider.Label.c_str());
        BarWidth();
        ImGuiSliderFlags flags = slider.Logarithmic ? ImGuiSliderFlags_Logarithmic : 0;
        if (!slider.Choices.empty()) {
            int choice = static_cast<int>(slider.Value);
            const char* shown = slider.Overridden && choice >= 0 && choice < static_cast<int>(slider.Choices.size()) ? slider.Choices[choice].c_str() : "game";
            if (ImGui::BeginCombo(slider.Label.c_str(), shown)) {
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
        if (ImGui::SliderFloat(slider.Label.c_str(), &slider.Value, slider.Min, slider.Max, slider.Overridden ? format : "game", flags)) {
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
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.5f);
        const char* format = control.Max >= 100 ? "%.0f" : "%.2f";
        if (ImGui::SliderFloat(control.Label, &control.Value, control.Min, control.Max, format))
            control.On = true;
        ImGui::PopID();
    }
}

void LightsGroup()
{
    for (int i = 0; i < lights::MaxLights; ++i) {
        lights::Light& light = lights::Rig()[i];
        ImGui::PushID(i);
        char title[24];
        snprintf(title, sizeof(title), "Light %d", i + 1);
        ImGui::Checkbox(title, &light.On);
        if (light.On) {
            ImGui::SameLine();
            ImGui::Checkbox("Spot", &light.Spot);
            int follow = static_cast<int>(light.Follow);
            BarWidth();
            if (ImGui::Combo("Follows", &follow, "Nothing\0P1\0P2\0Between them\0"))
                light.Follow = static_cast<lights::Follow>(follow);
            BarWidth();
            ImGui::SliderFloat3("Offset", light.Offset, -1000.0f, 1000.0f, "%.0f");
            BarWidth();
            ImGui::SliderFloat("Brightness", &light.Intensity, 100000.0f, 1000000000.0f, "%.0f", ImGuiSliderFlags_Logarithmic);
            Help("The game's stages run very bright: their own lights are between 30 and 100 million.");
            BarWidth();
            ImGui::SliderFloat("Reach", &light.Radius, 50.0f, 5000.0f, "%.0f cm");
            BarWidth();
            ImGui::ColorEdit3("Colour", light.Color, ImGuiColorEditFlags_NoInputs);
        }
        ImGui::PopID();
    }
}

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
        ImGui::Checkbox("Hide the HUD (F9)", &e.HideHud);
        changed |= ImGui::Checkbox("No bloom", &e.NoBloom);
        changed |= ImGui::Checkbox("No motion blur", &e.NoMotionBlur);
        changed |= ImGui::Checkbox("No depth of field", &e.NoDepthOfField);
        changed |= ImGui::Checkbox("No particles", &e.NoParticles);
        changed |= ImGui::Checkbox("No vignette", &e.NoVignette);
    }

    if (Group("Replay display")) {
        static int choices[switcher::DisplayRows];
        static const bool ready = [] {
            std::fill(std::begin(choices), std::end(choices), -1);
            return true;
        }();
        (void)ready;
        ImGui::TextColored(Dim, "The game's own display settings for replays.");
        for (int row = 0; row < switcher::DisplayRows; ++row) {
            const switcher::DisplayRow& info = switcher::DisplayRowInfo(row);
            ImGui::PushID(row);
            BarWidth();
            if (ImGui::BeginCombo(info.Label, choices[row] < 0 ? "Leave as it is" : switcher::DisplayOption(row, choices[row]))) {
                if (ImGui::Selectable("Leave as it is", choices[row] < 0))
                    choices[row] = -1;
                for (int option = 0; option < info.Options; ++option)
                    if (ImGui::Selectable(switcher::DisplayOption(row, option), option == choices[row]))
                        choices[row] = option;
                ImGui::EndCombo();
            }
            ImGui::PopID();
        }
        const bool busy = switcher::Running();
        ImGui::BeginDisabled(busy);
        if (ImGui::Button("Apply to the replay")) {
            std::string error;
            if (switcher::ApplyDisplay(choices, error))
                std::fill(std::begin(choices), std::end(choices), -1);
            else
                Notice(error.c_str());
        }
        ImGui::EndDisabled();
        Help("Opens the replay's own menu and sets these for you, which takes a few seconds with your mouse moving by itself. Don't touch the mouse or keyboard until the editor comes back. The game keeps them for your next replays too.");
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
        BarWidth();
        if (ImGui::Combo("Autofocus", &focus, "Off\0On P1\0On P2\0Between them\0From the camera\0"))
            postfx::FocusConfig().Mode = static_cast<postfx::Focus>(focus);
        PostFxGroup("Depth of field");
    }

    if (Group("Lens"))
        PostFxGroup("Lens");

    if (Group("Fog")) {
        DvarGroup("Fog");
        dvars::Colors& tint = dvars::Tint();
        BarWidth();
        if (ImGui::ColorEdit3("Fog colour", tint.Fog, ImGuiColorEditFlags_NoInputs)) {
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
        BarWidth();
        if (ImGui::ColorEdit3("Sun colour", tint.Sun, ImGuiColorEditFlags_NoInputs)) {
            tint.SunOverridden = true;
            dvars::TintChanged();
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
        changed = ImGui::Checkbox("No particles (stage and fight)", &e.NoParticles) || changed;
        if (changed)
            effects::RequestRefresh();
    }

    if (Group("Frame")) {
        scene::Settings& s = scene::Config();
        int frame = static_cast<int>(s.Frame);
        BarWidth();
        if (ImGui::Combo("Shape", &frame, "The game's own\0Fill the screen (no black bars)\0Fixed aspect ratio\0"))
            s.Frame = static_cast<scene::Frame>(frame);
        if (s.Frame == scene::Frame::Fixed) {
            BarWidth();
            ImGui::SliderFloat("Aspect ratio", &s.AspectRatio, 1.0f, 3.0f, "%.2f : 1");
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

    if (Group("Lighting rig"))
        LightsGroup();

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
    BarWidth();
    ImGui::InputInt("Framerate", &s.Fps, 0);
    s.Fps = std::clamp(s.Fps, 1, 1000);
    const auto& codecs = recorder::Codecs();
    s.CodecIndex = std::clamp(s.CodecIndex, 0, static_cast<int>(codecs.size()) - 1);
    BarWidth();
    if (ImGui::BeginCombo("Format", codecs[s.CodecIndex].Label)) {
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
    BarWidth();
    ImGui::SliderInt("Settle frames", &s.SettleFrames, 0, 16);
    Help("Frames to wait after switching stream, so the new look is really on screen. Raise it if streams bleed into each other. Lower is faster.");

    Section("Preview a look");
    int preview = static_cast<int>(s.Preview);
    BarWidth();
    if (ImGui::Combo("Look now", &preview, "Picture\0Greenscreen\0Stage only\0Fighters on black\0Depth\0Effects on black\0"))
        s.Preview = static_cast<recorder::Look>(preview);
    int keyColour = s.BlueKey ? 1 : 0;
    BarWidth();
    if (ImGui::Combo("Screen colour", &keyColour, "Green\0Blue\0"))
        s.BlueKey = keyColour == 1;
    Help("The colour behind the fighters in the Greenscreen look, on screen and in the video.");
    BarWidth();
    ImGui::SliderFloat("Depth near", &s.DepthNear, 0.0f, 2000.0f, "%.0f cm");
    Help("How close to the camera the depth look is pure white. Anything closer is white too.");
    BarWidth();
    ImGui::SliderFloat("Depth far", &s.DepthFar, 100.0f, 20000.0f, "%.0f cm", ImGuiSliderFlags_Logarithmic);
    Help("How far from the camera the depth look fades to black.");
    BarWidth();
    ImGui::SliderFloat("Depth curve", &s.DepthCurve, 0.25f, 4.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
    Help("Above 1 the grey falls off quickly behind the near distance, and below 1 it holds bright for longer.");
    const recorder::Pass shownPass = recorder::ActivePass();
    if ((shownPass == recorder::Pass::Depth || shownPass == recorder::Pass::Greenscreen) && !depth::Working()) {
        ImGui::TextColored(Dim, "Can't read the game's depth buffer yet.");
        BarWidth();
        ImGui::SliderInt("Key cut", &s.GreenDarkness, 2, 80);
        Help("Without the depth buffer, the video keys on how dark the background is. Dark clothing at a fighter's edge gets eaten if this is too high.");
    }

    Section("Motion blur");
    BarWidth();
    ImGui::SliderInt("Samples per frame", &s.MotionSamples, 1, 64);
    Help("1 = off. Each video frame averages this many real renders, so it takes that many times longer. 16 to 32 looks like film.");
    BarWidth();
    ImGui::SliderFloat("Shutter", &s.ShutterDegrees, 1.0f, 360.0f, "%.0f deg");
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
    int DragRange = 0; // 1 = start handle, 2 = end handle
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
    if (ImGui::SmallButton("+ Camera key (K)"))
        camera::AddKeyAtCurrentView();
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
        Help("The same, recording from the first key's exact frame to the last key's, with the streams set up in the RECORD tab.");
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
    if (g_notice[0]) {
        ImGui::SameLine();
        ImGui::TextColored(Dim, "%s", g_notice);
    }

    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float font = ImGui::GetFontSize();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float labelWidth = font * 6.5f;
    const float left = origin.x + labelWidth;
    const float width = std::max(position.x + size.x - left - font, 10.0f);
    const float available = position.y + size.y - origin.y - font * 0.5f;
    const float rulerHeight = font * 1.4f;
    const float cameraHeight = std::max(font * 1.5f, available * 0.25f);
    const float rangeHeight = font * 1.2f;
    const float speedHeight = std::max(available - rulerHeight - cameraHeight - rangeHeight, font * 1.5f);

    const float rulerTop = origin.y;
    const float cameraTop = rulerTop + rulerHeight;
    const float speedTop = cameraTop + cameraHeight;
    const float rangeTop = speedTop + speedHeight;
    const float bottom = rangeTop + rangeHeight;
    g_layout.Left = left;
    g_layout.Width = width;
    g_layout.RulerTop = rulerTop;
    g_layout.CameraTop = cameraTop;
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
    const ImU32 laneA = IM_COL32(24, 24, 30, 255), laneB = IM_COL32(30, 30, 37, 255), text = IM_COL32(200, 200, 205, 255), dim = IM_COL32(120, 120, 128, 255);
    draw->AddRectFilled({ left, cameraTop }, { left + width, speedTop }, laneA);
    draw->AddRectFilled({ left, speedTop }, { left + width, rangeTop }, laneB);
    draw->AddRectFilled({ left, rangeTop }, { left + width, bottom }, laneA);
    draw->AddText({ origin.x + font * 0.4f, cameraTop + font * 0.2f }, text, "Camera");
    draw->AddText({ origin.x + font * 0.4f, speedTop + font * 0.2f }, text, "Timescale");
    draw->AddText({ origin.x + font * 0.4f, rangeTop }, text, "Range");
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
        draw->AddLine({ x, cameraTop }, { x, bottom }, IM_COL32(60, 60, 70, 255));
        char label[48];
        snprintf(label, sizeof(label), "%.4gs  %d", t, static_cast<int>(std::lround(t * 60)));
        draw->AddText({ x + 3, rulerTop }, dim, label);
    }

    // Timescale lane: a guide at 1x, then the ramp's curve and keys.
    {
        const float oneX = SpeedToY(1.0, speedTop, speedHeight);
        draw->AddLine({ left, oneX }, { left + width, oneX }, IM_COL32(80, 80, 92, 255));
        draw->AddText({ left + 3, oneX - font }, dim, "1x");
        auto keys = game::SpeedKeys();
        if (!keys.empty()) {
            std::vector<ImVec2> points;
            for (float x = left; x <= left + width; x += 3.0f)
                points.push_back({ x, SpeedToY(std::clamp(cine::SampleSpeed(keys, XToTime(x, left, width)), MinSpeed, MaxSpeed), speedTop, speedHeight) });
            draw->AddPolyline(points.data(), static_cast<int>(points.size()), IM_COL32(230, 170, 60, 255), 0, 2.0f);
        }
        int hit = -1;
        for (int i = 0; i < static_cast<int>(keys.size()); ++i) {
            const ImVec2 at { TimeToX(keys[i].Time, left, width), SpeedToY(keys[i].Speed, speedTop, speedHeight) };
            if (std::fabs(mouse.x - at.x) < font * 0.5f && std::fabs(mouse.y - at.y) < font * 0.5f)
                hit = i;
            draw->AddCircleFilled(at, font * 0.32f, i == v.DragSpeed ? IM_COL32(255, 255, 255, 255) : IM_COL32(240, 185, 80, 255));
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
            draw->AddLine({ TimeToX(keys.front().Time, left, width), mid }, { TimeToX(keys.back().Time, left, width), mid }, IM_COL32(150, 60, 70, 255), 2.0f);
        int hit = -1;
        const float radius = std::min(font * 0.5f, cameraHeight * 0.4f);
        for (int i = 0; i < static_cast<int>(keys.size()); ++i) {
            const ImVec2 at { TimeToX(keys[i].Time, left, width), mid };
            if (std::fabs(mouse.x - at.x) < radius + 2 && std::fabs(mouse.y - at.y) < cameraHeight * 0.5f)
                hit = i;
            DrawDiamond(draw, at, radius, i == v.SelectedCamera ? IM_COL32(255, 255, 255, 255) : IM_COL32(230, 70, 80, 255), IM_COL32(0, 0, 0, 255));
        }
        const bool inCamera = hovered && inLanes && mouse.y >= cameraTop && mouse.y < speedTop;
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

    // Range lane
    {
        const float a = TimeToX(rec.RangeIn, left, width), b = TimeToX(rec.RangeOut, left, width);
        const ImU32 colour = rec.UseRange ? IM_COL32(70, 130, 90, 255) : IM_COL32(70, 70, 80, 255);
        draw->AddRectFilled({ a, rangeTop + 2 }, { b, bottom - 2 }, colour);
        draw->AddRectFilled({ a - 2, rangeTop }, { a + 2, bottom }, IM_COL32(220, 220, 225, 255));
        draw->AddRectFilled({ b - 2, rangeTop }, { b + 2, bottom }, IM_COL32(220, 220, 225, 255));
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
        draw->AddLine({ x, rulerTop }, { x, bottom }, IM_COL32(150, 110, 60, 255));
        char label[16];
        if (point.Type == replay::TypeRoundStart)
            snprintf(label, sizeof(label), "R%d", point.Round);
        else
            snprintf(label, sizeof(label), "End");
        draw->AddText({ x + 3, rulerTop }, IM_COL32(220, 170, 100, 255), label);
    }

    // Playhead
    const float head = TimeToX(clock, left, width);
    draw->AddLine({ head, rulerTop }, { head, bottom }, IM_COL32(255, 255, 255, 255), 2.0f);
    draw->AddTriangleFilled({ head - font * 0.35f, rulerTop }, { head + font * 0.35f, rulerTop }, { head, rulerTop + font * 0.5f }, IM_COL32(255, 255, 255, 255));
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

    // Pause and play, drawn since the font has no icons.
    const ImVec2 at = ImGui::GetCursorScreenPos();
    if (ImGui::Button("##playpause", { height * 1.4f, height }))
        t.Paused = !t.Paused;
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 c { at.x + height * 0.7f, at.y + height * 0.5f };
    g_layout.PlayButton = c;
    const float r = height * 0.26f;
    if (t.Paused) {
        draw->AddTriangleFilled({ c.x - r * 0.8f, c.y - r }, { c.x + r, c.y }, { c.x - r * 0.8f, c.y + r }, IM_COL32_WHITE);
    } else {
        draw->AddRectFilled({ c.x - r, c.y - r }, { c.x - r * 0.3f, c.y + r }, IM_COL32_WHITE);
        draw->AddRectFilled({ c.x + r * 0.3f, c.y - r }, { c.x + r, c.y + r }, IM_COL32_WHITE);
    }
    ImGui::SameLine();
    if (ImGui::Button("Next frame"))
        game::StepOneFrame();

    ImGui::SameLine();
    ImGui::SetNextItemWidth(size.x / 8);
    float speed = static_cast<float>(t.Speed);
    ImGui::BeginDisabled(t.UseSpeedRamp);
    if (ImGui::SliderFloat("##timescale", &speed, static_cast<float>(MinSpeed), static_cast<float>(MaxSpeed), "timescale %.3f", ImGuiSliderFlags_Logarithmic))
        t.Speed = speed;
    g_layout.SliderMin = ImGui::GetItemRectMin();
    g_layout.SliderMax = ImGui::GetItemRectMax();
    ImGui::EndDisabled();
    for (const float preset : { 0.01f, 0.1f, 0.25f, 0.5f, 1.0f }) {
        ImGui::SameLine();
        char label[16];
        snprintf(label, sizeof(label), "%gx", preset);
        if (ImGui::SmallButton(label)) {
            t.Speed = preset;
            t.UseSpeedRamp = false;
        }
        g_layout.OneX = { (ImGui::GetItemRectMin().x + ImGui::GetItemRectMax().x) * 0.5f, (ImGui::GetItemRectMin().y + ImGui::GetItemRectMax().y) * 0.5f };
    }

    // Jump to the start of a round. Round one has no point of its own: it starts at the first saved point.
    const std::vector<replay::SavedPoint> points = replay::SavedPoints();
    int roundsShown = 0;
    const auto roundButton = [&](int round, uint32_t frame) {
        ImGui::SameLine(0, roundsShown == 0 ? ImGui::GetStyle().ItemSpacing.x * 4 : -1.0f);
        ++roundsShown;
        char label[16];
        snprintf(label, sizeof(label), "R%d", round);
        if (ImGui::SmallButton(label))
            game::SeekToRound(frame);
    };
    bool hasFirstRound = false;
    for (const replay::SavedPoint& point : points)
        hasFirstRound = hasFirstRound || (point.Type == replay::TypeRoundStart && point.Round <= 1);
    ImGui::SameLine(0, ImGui::GetStyle().ItemSpacing.x * 4);
    if (ImGui::SmallButton("Start"))
        game::SeekToStart();
    ++roundsShown;
    if (!points.empty() && !hasFirstRound)
        roundButton(1, points.front().Frame);
    for (const replay::SavedPoint& point : points)
        if (point.Type == replay::TypeRoundStart)
            roundButton(point.Round, point.Frame);

    const game::Rates rates = game::MeasuredRates();
    char info[160];
    const double clock = game::Clock();
    snprintf(info, sizeof(info), "tick %d   %.3f s   %.1f ticks/s   %.0f fps%s%s", static_cast<int>(std::lround(clock * 60)), clock, rates.TicksPerSecond,
             rates.FramesPerSecond, game::Seeking() ? "   running ahead" : "", recorder::Active() ? "   REC" : "");
    const float textWidth = ImGui::CalcTextSize(info).x;
    ImGui::SameLine(size.x - textWidth - ImGui::GetStyle().WindowPadding.x);
    ImGui::TextUnformatted(info);
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
    const char* label = "tekken8mvm 1.0  |  TEKKEN 8";
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

    static const char* const names[TabCount] = { "REPLAYS", "CAMERA", "VISUALS", "RECORD" };
    const float tabWidth = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * (TabCount - 1)) / TabCount;
    for (int i = 0; i < TabCount; ++i) {
        if (i > 0)
            ImGui::SameLine();
        const bool selected = g_tab == i;
        if (selected)
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.78f, 0.17f, 0.22f, 1));
        if (ImGui::Button(names[i], { tabWidth, ImGui::GetFrameHeight() * 1.2f }))
            g_tab = i;
        g_layout.Tabs[i] = { (ImGui::GetItemRectMin().x + ImGui::GetItemRectMax().x) * 0.5f, (ImGui::GetItemRectMin().y + ImGui::GetItemRectMax().y) * 0.5f };
        if (selected)
            ImGui::PopStyleColor();
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
        default:
            RecordTab();
            break;
        }
    }
    ImGui::EndChild();
    ImGui::End();
}

} // namespace

void SetTab(int tab)
{
    g_tab = tab;
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
            const float mid = (g_layout.CameraTop + g_layout.SpeedTop) * 0.5f;
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
        drag([keyAt] { return keyAt(1); }, [keyAt] { return ImVec2(TimeToX(3.6, g_layout.Left, g_layout.Width), (g_layout.CameraTop + g_layout.SpeedTop) * 0.5f); });
        check("dragging a camera key moves it", [] {
            const auto keys = camera::Keys();
            return keys.size() == countBefore && keys[1].Time > before + 0.3 && std::fabs(keys[1].Time - 3.6) < 0.1;
        });
        check("camera keys stay in time order", [] {
            const auto keys = camera::Keys();
            return std::is_sorted(keys.begin(), keys.end(), [](const cine::CameraKey& a, const cine::CameraKey& b) { return a.Time < b.Time; });
        });
        drag([keyAt] { return keyAt(1); }, [] { return ImVec2(TimeToX(6.0, g_layout.Left, g_layout.Width), (g_layout.CameraTop + g_layout.SpeedTop) * 0.5f); });
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
        moveTo([] { return ImVec2(g_layout.Left + g_layout.Width * 0.5f, (g_layout.CameraTop + g_layout.SpeedTop) * 0.5f); });
        steps.push_back([&io] { io.AddMouseWheelEvent(0, 3); });
        wait(2);
        check("the wheel zooms the timeline", [] { return g_timeline.End - g_timeline.Start < span * 0.8; });
        steps.push_back([] { span = g_timeline.Start; });
        drag([] { return ImVec2(g_layout.Left + g_layout.Width * 0.5f, (g_layout.CameraTop + g_layout.SpeedTop) * 0.5f); },
             [] { return ImVec2(g_layout.Left + g_layout.Width * 0.3f, (g_layout.CameraTop + g_layout.SpeedTop) * 0.5f); }, 2);
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
        steps.push_back([] { mod::Log("[uitest] finished"); });
    }

    io.AddMousePosEvent(pointer.x, pointer.y);
    if (at < steps.size())
        steps[at++]();
    return at >= steps.size();
}

void ApplyStyle()
{
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 0;
    style.FrameRounding = 3;
    style.GrabRounding = 3;
    style.TabRounding = 3;
    style.WindowBorderSize = 0;
    style.FramePadding = ImVec2(7, 3);
    style.ItemSpacing = ImVec2(7, 5);
    style.WindowPadding = ImVec2(9, 7);
    ImVec4* c = style.Colors;
    c[ImGuiCol_WindowBg] = ImVec4(0.055f, 0.055f, 0.07f, 1.0f);
    c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_PopupBg] = ImVec4(0.09f, 0.09f, 0.11f, 0.98f);
    c[ImGuiCol_MenuBarBg] = ImVec4(0.10f, 0.10f, 0.125f, 1);
    c[ImGuiCol_Border] = ImVec4(0.25f, 0.25f, 0.30f, 0.6f);
    c[ImGuiCol_TitleBg] = c[ImGuiCol_TitleBgActive] = c[ImGuiCol_TitleBgCollapsed] = ImVec4(0.11f, 0.11f, 0.14f, 1);
    c[ImGuiCol_FrameBg] = ImVec4(0.15f, 0.15f, 0.19f, 1);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.21f, 0.21f, 0.26f, 1);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.26f, 0.26f, 0.32f, 1);
    c[ImGuiCol_Button] = ImVec4(0.19f, 0.19f, 0.24f, 1);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.62f, 0.15f, 0.19f, 1);
    c[ImGuiCol_ButtonActive] = ImVec4(0.78f, 0.17f, 0.22f, 1);
    c[ImGuiCol_Header] = ImVec4(0.17f, 0.17f, 0.21f, 1);
    c[ImGuiCol_HeaderHovered] = ImVec4(0.62f, 0.15f, 0.19f, 1);
    c[ImGuiCol_HeaderActive] = ImVec4(0.78f, 0.17f, 0.22f, 1);
    c[ImGuiCol_Tab] = ImVec4(0.13f, 0.13f, 0.17f, 1);
    c[ImGuiCol_TabHovered] = ImVec4(0.62f, 0.15f, 0.19f, 1);
    c[ImGuiCol_TabSelected] = ImVec4(0.78f, 0.17f, 0.22f, 1);
    c[ImGuiCol_SliderGrab] = ImVec4(0.78f, 0.17f, 0.22f, 1);
    c[ImGuiCol_SliderGrabActive] = ImVec4(0.95f, 0.25f, 0.30f, 1);
    c[ImGuiCol_CheckMark] = ImVec4(0.95f, 0.25f, 0.30f, 1);
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
}

void Draw()
{
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    ImGui::GetBackgroundDrawList()->AddRectFilled({ 0, 0 }, display, IM_COL32_BLACK);

    MenuBar();
    const float menuHeight = ImGui::GetFrameHeight();

    // The game keeps its shape in the top left. A bit smaller than IWXMVM's, since this timeline has more lanes.
    const float aspect = overlay::GameAspect() > 0 ? overlay::GameAspect() : display.x / display.y;
    const float barHeight = ImGui::GetFrameHeight() + ImGui::GetStyle().WindowPadding.y * 2;
    const float viewWidth = std::floor(display.x * 0.74f);
    const float viewHeight = std::floor(viewWidth / aspect);
    const ImVec2 viewMin { 0, menuHeight }, viewMax { viewWidth, menuHeight + viewHeight };

    if (const uint64_t texture = overlay::GameTexture())
        ImGui::GetBackgroundDrawList()->AddImage(static_cast<ImTextureID>(texture), viewMin, viewMax);

    const ImVec2 mouse = ImGui::GetMousePos();
    overlay::SetGameViewHovered(mouse.x >= viewMin.x && mouse.x < viewMax.x && mouse.y >= viewMin.y && mouse.y < viewMax.y && !ImGui::IsAnyItemActive()
                                && !ImGui::GetIO().WantTextInput);

    // A short reminder over the picture of what the view is doing right now.
    const char* hint = nullptr;
    switch (camera::Config().Mode) {
    case camera::Mode::Free:
        hint = "FREE CAM   hold right mouse to look, WASD to fly, K to add a key, Space to play";
        break;
    case camera::Mode::Dolly:
        hint = "DOLLY   playing the camera path";
        break;
    case camera::Mode::Attach:
        hint = "ATTACHED   right mouse and wheel for orbit";
        break;
    default:
        break;
    }
    if (hint) {
        ImDrawList* background = ImGui::GetBackgroundDrawList();
        const ImVec2 at { viewMin.x + 10, viewMin.y + 8 };
        background->AddText({ at.x + 1, at.y + 1 }, IM_COL32(0, 0, 0, 220), hint);
        background->AddText(at, IM_COL32(255, 255, 255, 235), hint);
    }

    if (camera::Config().RuleOfThirds)
        DrawThirds(ImGui::GetBackgroundDrawList(), viewMin, viewMax);
    presets::UndoWatch(!ImGui::IsAnyItemActive() && !ImGui::IsMouseDown(ImGuiMouseButton_Left));

    g_layout.ViewCenter = { viewWidth * 0.5f, menuHeight + viewHeight * 0.5f };
    g_layout.PanelCenter = { (viewWidth + display.x) * 0.5f, menuHeight + viewHeight * 0.5f };

    RightPanel({ viewWidth, menuHeight }, { display.x - viewWidth, viewHeight });
    ControlBar({ 0, menuHeight + viewHeight }, { display.x, barHeight });
    const float timelineTop = menuHeight + viewHeight + barHeight;
    Timeline({ 0, timelineTop }, { display.x, std::max(display.y - timelineTop, 1.0f) });
}

void ShowNotice(const char* text)
{
    Notice(text);
}

void DrawOsd()
{
    if (camera::Config().RuleOfThirds)
        DrawThirds(ImGui::GetForegroundDrawList(), { 0, 0 }, ImGui::GetIO().DisplaySize);
    std::string text;
    const game::TimeControl& t = game::Time();
    if (camera::Config().Mode == camera::Mode::Free)
        text += "FREE CAM   ";
    else if (camera::Config().Mode == camera::Mode::Dolly)
        text += "DOLLY   ";
    else if (camera::Config().Mode == camera::Mode::Attach)
        text += "ATTACHED   ";
    if (t.Paused)
        text += "PAUSED   ";
    else if (t.Speed != 1.0) {
        char speed[32];
        snprintf(speed, sizeof(speed), "%.3gx   ", t.Speed);
        text += speed;
    }
    if (recorder::Active()) {
        char frames[48];
        snprintf(frames, sizeof(frames), "REC %llu", static_cast<unsigned long long>(recorder::Frames()));
        text += frames;
    } else if (recorder::Finishing()) {
        text += "SAVING";
    }
    if (text.empty())
        return;
    ImGui::GetForegroundDrawList()->AddText(ImVec2(13, 13), IM_COL32(0, 0, 0, 200), text.c_str());
    ImGui::GetForegroundDrawList()->AddText(ImVec2(12, 12), IM_COL32(255, 90, 90, 255), text.c_str());
}

} // namespace menu
