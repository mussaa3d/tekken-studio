// The studio's editor, drawn on the game's own ImGui drawer, in Tekken Studio's look. Two ways to use it:
//   Simple: a playback bar along the bottom, for watching a replay like any other (pause, slow motion, step). Home shows and hides it.
//   Studio: the bar, a timeline with camera keys, and the panel with the Replays, Camera, Time and Record tabs. Insert opens and closes it.
// The picture isn't shrunk into a corner as in the Tekken 8 studio: the game draws it itself, full size, and the panels sit over it.
#include "studio.h"

#include "bluespark.h"
#include "camera.h"

#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <cmath>
#include <vector>

#include <atomic>
#include <imgui.h>
#include <imgui_internal.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>

namespace studio {

namespace {

// Tekken Studio's palette.
namespace pal {
constexpr uint32_t Bg = 0x1C1C1C, Surface = 0x242424, Raised = 0x2E2E2E, Border = 0x383838, Text = 0xD6D6D6, Muted = 0x8C8C8C, Accent = 0xCFCFCF, Ok = 0x7FB58F,
                   Warning = 0xD4AA55, Bad = 0xD46A62, Blue = 0x3F9BD1, Path = 0xFFCC00;
}
ImVec4 Colour(uint32_t rgb, float alpha = 1.0f)
{
    return { ((rgb >> 16) & 0xFF) / 255.0f, ((rgb >> 8) & 0xFF) / 255.0f, (rgb & 0xFF) / 255.0f, alpha };
}
ImU32 Hex(uint32_t rgb, int alpha = 255)
{
    return IM_COL32((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF, alpha);
}
constexpr uint32_t Mix(uint32_t from, uint32_t to, float amount)
{
    const auto channel = [&](int shift) {
        const float a = static_cast<float>((from >> shift) & 0xFF), b = static_cast<float>((to >> shift) & 0xFF);
        return static_cast<uint32_t>(a + (b - a) * amount) << shift;
    };
    return channel(16) | channel(8) | channel(0);
}

void ApplyStyle(ImGuiStyle& style)
{
    style.WindowBorderSize = 0;
    style.WindowRounding = 0;
    style.FrameRounding = 2;
    style.GrabRounding = 2;
    const uint32_t hover = Mix(pal::Raised, pal::Text, 0.08f), selected = Mix(pal::Raised, pal::Accent, 0.30f), held = Mix(pal::Raised, pal::Accent, 0.40f);
    ImVec4* c = style.Colors;
    c[ImGuiCol_Text] = Colour(pal::Text);
    c[ImGuiCol_TextDisabled] = Colour(pal::Muted);
    c[ImGuiCol_WindowBg] = Colour(pal::Bg, 0.96f);
    c[ImGuiCol_ChildBg] = Colour(pal::Bg, 0.0f);
    c[ImGuiCol_PopupBg] = Colour(pal::Surface, 0.98f);
    c[ImGuiCol_Border] = Colour(pal::Border);
    c[ImGuiCol_FrameBg] = Colour(pal::Surface);
    c[ImGuiCol_FrameBgHovered] = Colour(Mix(pal::Surface, pal::Text, 0.07f));
    c[ImGuiCol_FrameBgActive] = Colour(Mix(pal::Surface, pal::Text, 0.12f));
    c[ImGuiCol_TitleBg] = Colour(pal::Surface);
    c[ImGuiCol_TitleBgActive] = Colour(pal::Raised);
    c[ImGuiCol_ScrollbarBg] = Colour(pal::Bg, 0.6f);
    c[ImGuiCol_ScrollbarGrab] = Colour(pal::Raised);
    c[ImGuiCol_CheckMark] = Colour(pal::Accent);
    c[ImGuiCol_SliderGrab] = Colour(pal::Accent);
    c[ImGuiCol_SliderGrabActive] = Colour(Mix(pal::Accent, 0xFFFFFF, 0.12f));
    c[ImGuiCol_Button] = Colour(pal::Raised);
    c[ImGuiCol_ButtonHovered] = Colour(hover);
    c[ImGuiCol_ButtonActive] = Colour(held);
    c[ImGuiCol_Header] = Colour(selected);
    c[ImGuiCol_HeaderHovered] = Colour(hover);
    c[ImGuiCol_HeaderActive] = Colour(held);
    c[ImGuiCol_Separator] = Colour(pal::Border);
    c[ImGuiCol_Tab] = Colour(pal::Surface);
    c[ImGuiCol_TabHovered] = Colour(hover);
    c[ImGuiCol_TabSelected] = Colour(selected);
    c[ImGuiCol_TextSelectedBg] = Colour(pal::Accent, 0.30f);
}

EditorMode g_mode = EditorMode::Simple;
int g_selectedKey = -1;
int g_dragKey = -1;
int g_dragAxis = -1;
bool g_keysCaptured = false;
std::string g_notice;
double g_noticeUntil = 0;

struct ReplayFile {
    std::filesystem::path Path;
    std::string Name, When;
    uintmax_t Size = 0;
    std::filesystem::file_time_type Written;
};
std::vector<ReplayFile> g_replays;

void Notice(const std::string& text)
{
    g_notice = text;
    g_noticeUntil = ImGui::GetTime() + 4.0;
}

void RefreshReplays()
{
    g_replays.clear();
    std::error_code ignored;
    const std::filesystem::path folder = Ttt2ReplaysFolder();
    for (const auto& entry : std::filesystem::recursive_directory_iterator(folder, ignored)) {
        if (!entry.is_regular_file(ignored) || entry.path().extension() != ".ttt2replay")
            continue;
        ReplayFile file;
        file.Path = entry.path();
        file.Name = std::filesystem::relative(entry.path(), folder, ignored).string();
        file.Size = entry.file_size(ignored);
        file.Written = entry.last_write_time(ignored);
        g_replays.push_back(std::move(file));
    }
    std::sort(g_replays.begin(), g_replays.end(), [](const ReplayFile& a, const ReplayFile& b) { return a.Written > b.Written; });
}

void Section(const char* title)
{
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, Colour(pal::Muted));
    ImGui::TextUnformatted(title);
    ImGui::PopStyleColor();
    ImGui::Separator();
}

void Help(const char* text)
{
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

// ----- The tabs

void ReplaysTab()
{
    static bool loaded = false;
    if (!loaded) {
        loaded = true;
        RefreshReplays();
    }
    ImGui::TextWrapped("BlueSpark records every offline Versus match and every online match by itself. They're listed here, newest first.");
    if (ImGui::SmallButton("Refresh"))
        RefreshReplays();
    ImGui::SameLine();
    if (ImGui::SmallButton("Open the folder"))
        ShellExecuteW(nullptr, L"open", Ttt2ReplaysFolder().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    ImGui::Text("%d replays", static_cast<int>(g_replays.size()));
    if (ImGui::BeginChild("replays", ImVec2(0, ImGui::GetContentRegionAvail().y * 0.55f), ImGuiChildFlags_Borders)) {
        for (size_t i = 0; i < g_replays.size(); ++i) {
            const ReplayFile& file = g_replays[i];
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::SmallButton("Watch")) {
                uint8_t* base = GuestBase();
                if (base && Ttt2ReplayChoose(base, file.Path.string()))
                    Notice("Starting " + file.Name);
                else
                    Notice("BlueSpark wouldn't start it from here. Open it from its own REPLAYS tab.");
            }
            ImGui::SameLine();
            ImGui::Text("%s  (%d KB)", file.Name.c_str(), static_cast<int>(file.Size / 1024));
            ImGui::PopID();
        }
        if (g_replays.empty())
            ImGui::TextDisabled("None yet. Play a Versus match and it shows up here.");
    }
    ImGui::EndChild();
    ImGui::TextWrapped("While one plays, BlueSpark's own controls still work: Select + Left / Right changes round, Select + Down opens the points to jump to, "
                       "and Select + LB takes over a fighter.");
}

void CameraTab()
{
    camera::Settings& s = camera::Config();
    Section("Camera");
    const int mode = static_cast<int>(s.Mode);
    if (ImGui::RadioButton("Game", mode == 0))
        camera::SetMode(camera::Mode::Game);
    ImGui::SameLine();
    if (ImGui::RadioButton("Free (F5)", mode == 1))
        camera::SetMode(camera::Mode::Free);
    ImGui::SameLine();
    if (ImGui::RadioButton("Dolly (F6)", mode == 2))
        camera::SetMode(camera::Mode::Dolly);
    if (s.Mode == camera::Mode::Free) {
        ImGui::SliderFloat("Field of view", &s.Free.Fov, 5.0f, 130.0f, "%.1f");
        ImGui::SliderFloat("Fly speed", &s.FlySpeed, 100.0f, 30000.0f, "%.0f", ImGuiSliderFlags_Logarithmic);
        ImGui::SliderFloat("Look speed", &s.LookSpeed, 0.02f, 0.6f, "%.2f");
        ImGui::TextDisabled("Hold the right mouse button over the picture to look.");
        ImGui::TextDisabled("W A S D fly, Q E down and up, Shift faster, Ctrl slower.");
        ImGui::TextDisabled("Wheel zooms. K adds a key at this moment.");
    }

    Section("Dolly path");
    int style = static_cast<int>(s.Style);
    if (ImGui::Combo("Style", &style, "Smooth\0Linear\0"))
        s.Style = static_cast<camera::PathStyle>(style);
    ImGui::Checkbox("Show the path in the picture", &s.ShowPath);
    std::vector<camera::Key>& keys = camera::Keys();
    if (ImGui::SmallButton("Add a key (K)"))
        camera::AddKeyAtView(Clock());
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear") && !keys.empty()) {
        keys.clear();
        g_selectedKey = -1;
    }
    ImGui::SameLine();
    ImGui::Text("%d keys", static_cast<int>(keys.size()));
    Help("Fly the free camera somewhere, go to another moment of the fight, fly somewhere else, and add a key each time. Dolly then flies through them "
         "as the fight plays. The path follows the fight's own clock, so it slows and stops with it.");

    static char name[64] = "path1";
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.45f);
    ImGui::InputText("##pathname", name, sizeof(name));
    ImGui::SameLine();
    if (ImGui::SmallButton("Save"))
        Notice(camera::SavePath(name) ? "Path saved" : "Couldn't save the path");
    for (const std::string& saved : camera::SavedPaths()) {
        ImGui::SameLine();
        if (ImGui::SmallButton((saved + "##load").c_str())) {
            camera::LoadPath(saved);
            g_selectedKey = -1;
        }
    }

    if (g_selectedKey >= 0 && g_selectedKey < static_cast<int>(keys.size())) {
        Section("Key");
        camera::Key& key = keys[g_selectedKey];
        ImGui::Text("Key %d", g_selectedKey + 1);
        float time = static_cast<float>(key.Time);
        if (ImGui::DragFloat("Time", &time, 0.01f, 0.0f, 0.0f, "%.3f s"))
            key.Time = std::max(time, 0.0f);
        ImGui::DragFloat3("Place", &key.Pose.Eye.X, 10.0f, 0.0f, 0.0f, "%.0f");
        ImGui::DragFloat("Yaw", &key.Pose.Yaw, 0.3f, 0.0f, 0.0f, "%.1f");
        ImGui::DragFloat("Pitch", &key.Pose.Pitch, 0.3f, -89.0f, 89.0f, "%.1f");
        ImGui::DragFloat("Field of view##key", &key.Pose.Fov, 0.2f, 5.0f, 130.0f, "%.1f");
        if (ImGui::SmallButton("From the view")) {
            camera::View now;
            if (camera::Current(now)) {
                const double keep = key.Time;
                key.Pose = camera::ToPose(now);
                key.Time = keep;
            }
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Go there")) {
            s.Free = key.Pose;
            s.Mode = camera::Mode::Free;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Delete")) {
            keys.erase(keys.begin() + g_selectedKey);
            g_selectedKey = -1;
        }
    }

    camera::View view;
    if (camera::Current(view)) {
        Section("Position");
        ImGui::TextDisabled("X %.0f  Y %.0f  Z %.0f   FOV %.1f", view.Eye.X, view.Eye.Y, view.Eye.Z, view.Fov);
    }
}

void TimeTab()
{
    TimeControl& t = Time();
    Section("Time");
    if (!CanHold() && !ReplayPlaying()) {
        ImGui::TextWrapped("Pause, slow motion and frame steps work in Practice and in an offline fight (Arcade, Versus). Start one first.");
        return;
    }
    ImGui::Checkbox("Paused (Space)", &t.Paused);
    float speed = static_cast<float>(t.Speed);
    if (ImGui::SliderFloat("Speed", &speed, 0.05f, 1.0f, "%.2fx", ImGuiSliderFlags_Logarithmic))
        t.Speed = speed;
    for (const float preset : { 0.05f, 0.1f, 0.25f, 0.5f, 1.0f }) {
        char label[16];
        snprintf(label, sizeof(label), "%gx", preset);
        if (ImGui::SmallButton(label))
            t.Speed = preset;
        ImGui::SameLine();
    }
    ImGui::NewLine();
    if (ImGui::SmallButton("Step one frame (.)"))
        StepOneFrame();
    ImGui::TextWrapped("Slow motion lets one frame run in every few, so the movement is the game's real 60 frames, shown slower. In Practice and offline "
                       "fights the fight is held through BlueSpark's own hold, and the sound pauses with it. In a replay the studio leaves the game's "
                       "frame out instead, and the replay stays in step: it's keyed to the fight's frames. LB still goes back 3 seconds there.");
}

void RecordTab()
{
    Section("Record");
    const bool fight = CanHold() || ReplayPlaying();
    ImGui::BeginDisabled(!fight);
    if (ImGui::Button("Record (F9)", { ImGui::GetFontSize() * 10, ImGui::GetFrameHeight() * 1.4f }))
        RecorderStart();
    ImGui::EndDisabled();
    if (!fight)
        ImGui::TextWrapped("Start a replay, Practice or an offline fight first.");
    const std::string message = RecorderMessage();
    if (!message.empty())
        ImGui::TextWrapped("%s", message.c_str());
    if (ImGui::SmallButton("Open the folder")) {
        std::error_code ignored;
        std::filesystem::create_directories(RecorderFolder(), ignored);
        ShellExecuteW(nullptr, L"open", RecorderFolder().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
    ImGui::Spacing();
    ImGui::TextWrapped("F9 starts and stops. The video is 60 frames a second at the window's size, every game frame exactly once: the game runs one frame, the "
                       "picture is taken, then the next, however long each takes. So it plays slower than real time while it records. At a slow speed "
                       "each frame is written several times, which is slow motion from the game's real frames. The camera path keeps flying.");
    ImGui::TextWrapped("While it records the studio draws nothing and BlueSpark's hints are off, so the video is the game's picture alone. Keep the game's "
                       "window in front and uncovered: the picture is taken off the screen. There's no sound in the video.");
}

// ----- The path in the picture, with handles on the selected key

void PathInPicture(ImDrawList* draw, ImVec2 size, bool interactive)
{
    camera::Settings& s = camera::Config();
    std::vector<camera::Key>& keys = camera::Keys();
    camera::View view;
    if (!s.ShowPath || s.Mode == camera::Mode::Dolly || keys.empty() || !camera::Current(view))
        return;
    const float aspect = size.x / std::max(size.y, 1.0f);
    auto project = [&](camera::Vec3 p, ImVec2& out) {
        float x, y;
        if (!camera::Project(view, aspect, p, x, y))
            return false;
        out = { x * size.x, y * size.y };
        return true;
    };
    const ImU32 pathColour = Hex(pal::Path, 220);
    if (keys.size() >= 2) {
        const double start = keys.front().Time, end = keys.back().Time;
        const int steps = std::clamp(static_cast<int>(keys.size()) * 48, 96, 600);
        ImVec2 previous;
        bool have = false;
        for (int i = 0; i <= steps; ++i) {
            const camera::Pose pose = camera::Sample(keys, start + (end - start) * i / steps, s.Style);
            ImVec2 at;
            const bool ok = project(pose.Eye, at);
            if (ok && have)
                draw->AddLine(previous, at, pathColour, 2.5f);
            previous = at;
            have = ok;
        }
        const double now = Clock();
        ImVec2 at;
        if (now >= start && now <= end && project(camera::Sample(keys, now, s.Style).Eye, at)) {
            draw->AddCircleFilled(at, 5.0f, IM_COL32_WHITE);
            draw->AddCircle(at, 5.0f, IM_COL32(0, 0, 0, 200), 0, 1.5f);
        }
    }
    const ImVec2 mouse = ImGui::GetMousePos();
    const bool free = interactive && !ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow) && !ImGui::IsAnyItemActive() && !ImGui::IsMouseDown(ImGuiMouseButton_Right);

    // The selected key's arrows, along the world's X, up and Z.
    int hoveredAxis = -1;
    if (g_selectedKey >= 0 && g_selectedKey < static_cast<int>(keys.size())) {
        camera::Key& key = keys[g_selectedKey];
        ImVec2 centre;
        if (project(key.Pose.Eye, centre)) {
            const float dx = key.Pose.Eye.X - view.Eye.X, dy = key.Pose.Eye.Y - view.Eye.Y, dz = key.Pose.Eye.Z - view.Eye.Z;
            const float length = std::sqrt(dx * dx + dy * dy + dz * dz) * 0.16f; // about the same size on screen at any distance
            const camera::Vec3 axes[3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
            const ImU32 colours[3] = { IM_COL32(235, 70, 70, 255), IM_COL32(80, 210, 90, 255), IM_COL32(70, 130, 255, 255) };
            const char* names[3] = { "X", "up", "Z" };
            float best = 8.0f;
            for (int a = 0; a < 3; ++a) {
                ImVec2 tip;
                if (!project({ key.Pose.Eye.X + axes[a].X * length, key.Pose.Eye.Y + axes[a].Y * length, key.Pose.Eye.Z + axes[a].Z * length }, tip))
                    continue;
                const bool lit = g_dragAxis == a;
                draw->AddLine(centre, tip, colours[a], lit ? 5.0f : 3.0f);
                draw->AddCircleFilled(tip, lit ? 7.0f : 5.5f, colours[a]);
                draw->AddText({ tip.x + 8, tip.y - 7 }, colours[a], names[a]);
                const float sx = tip.x - centre.x, sy = tip.y - centre.y, pixels = std::sqrt(sx * sx + sy * sy);
                if (pixels > 4) {
                    const float t = std::clamp(((mouse.x - centre.x) * sx + (mouse.y - centre.y) * sy) / (pixels * pixels), 0.0f, 1.0f);
                    const float ex = centre.x + sx * t - mouse.x, ey = centre.y + sy * t - mouse.y, distance = std::sqrt(ex * ex + ey * ey);
                    if (free && distance < best) {
                        best = distance;
                        hoveredAxis = a;
                    }
                    if (g_dragAxis == a) {
                        const ImVec2 delta = ImGui::GetIO().MouseDelta;
                        const float along = (delta.x * sx + delta.y * sy) / pixels * (length / pixels);
                        key.Pose.Eye.X += axes[a].X * along;
                        key.Pose.Eye.Y += axes[a].Y * along;
                        key.Pose.Eye.Z += axes[a].Z * along;
                    }
                }
            }
            if (free && hoveredAxis >= 0 && g_dragAxis < 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                g_dragAxis = hoveredAxis;
        }
    }
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
        g_dragAxis = -1;

    // A little camera at each key, with its number.
    int hoveredKey = -1;
    float nearest = 14.0f;
    for (int i = 0; i < static_cast<int>(keys.size()); ++i) {
        ImVec2 apex;
        if (!project(keys[i].Pose.Eye, apex))
            continue;
        const float distance = std::hypot(mouse.x - apex.x, mouse.y - apex.y);
        if (free && distance < nearest) {
            nearest = distance;
            hoveredKey = i;
        }
        const bool selected = i == g_selectedKey;
        const ImU32 colour = selected ? IM_COL32_WHITE : pathColour;
        // Where the key looks: a short line from it.
        const camera::View look = camera::ToView(keys[i].Pose);
        const float scale = 0.07f;
        ImVec2 ahead;
        if (project({ look.Eye.X + (look.Target.X - look.Eye.X) * scale, look.Eye.Y + (look.Target.Y - look.Eye.Y) * scale, look.Eye.Z + (look.Target.Z - look.Eye.Z) * scale }, ahead))
            draw->AddLine(apex, ahead, colour, selected ? 2.5f : 1.8f);
        draw->AddCircleFilled(apex, selected ? 6.0f : 4.5f, colour);
        draw->AddCircle(apex, selected ? 6.0f : 4.5f, IM_COL32(0, 0, 0, 200), 0, 1.2f);
        char label[8];
        snprintf(label, sizeof(label), "%d", i + 1);
        draw->AddText({ apex.x + 8, apex.y + 4 }, colour, label);
    }
    if (free && g_dragAxis < 0 && hoveredAxis < 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        g_selectedKey = hoveredKey;
}

// Research: crosses where the game says its four fighters stand (the effects' copy of the camera keeps them, 0x82DE69E0), to check
// that the studio's projection puts world points where the game draws them.
bool g_markers = false;
void FighterMarkers(ImDrawList* draw, ImVec2 size)
{
    camera::View view;
    if (!g_markers || !camera::Current(view) || !Readable(0x82DE69E0, 64))
        return;
    for (int i = 0; i < 4; ++i) {
        const camera::Vec3 at { ReadFloat(0x82DE69E0 + i * 16), ReadFloat(0x82DE69E4 + i * 16), ReadFloat(0x82DE69E8 + i * 16) };
        float x, y;
        if (!camera::Project(view, size.x / size.y, at, x, y))
            continue;
        const ImVec2 p { x * size.x, y * size.y };
        draw->AddLine({ p.x - 14, p.y }, { p.x + 14, p.y }, IM_COL32(0, 255, 255, 255), 2.0f);
        draw->AddLine({ p.x, p.y - 14 }, { p.x, p.y + 14 }, IM_COL32(0, 255, 255, 255), 2.0f);
        // And the floor under it.
        if (camera::Project(view, size.x / size.y, { at.X, 0, at.Z }, x, y))
            draw->AddCircle({ x * size.x, y * size.y }, 8.0f, IM_COL32(255, 0, 255, 255), 0, 2.0f);
    }
}

// ----- The bar and the timeline

const float kSpeeds[] = { 0.05f, 0.1f, 0.25f, 0.5f, 1.0f };

void PlaybackBar(ImVec2 position, ImVec2 size, bool studio)
{
    TimeControl& t = Time();
    ImGui::SetNextWindowPos(position);
    ImGui::SetNextWindowSize(size);
    ImGui::Begin("##bar", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing);
    const bool can = CanHold() || ReplayPlaying();
    ImGui::BeginDisabled(!can);
    if (ImGui::Button(t.Paused ? "Play" : "Pause", { ImGui::GetFontSize() * 4, 0 }))
        t.Paused = !t.Paused;
    ImGui::SameLine();
    if (ImGui::Button("Step"))
        StepOneFrame();
    ImGui::SameLine();
    for (const float preset : kSpeeds) {
        char label[16];
        snprintf(label, sizeof(label), "%gx", preset);
        const bool on = std::fabs(static_cast<float>(t.Speed) - preset) < 0.001f;
        if (on)
            ImGui::PushStyleColor(ImGuiCol_Button, Colour(Mix(pal::Raised, pal::Accent, 0.40f)));
        if (ImGui::Button(label))
            t.Speed = preset;
        if (on)
            ImGui::PopStyleColor();
        ImGui::SameLine();
    }
    ImGui::EndDisabled();
    const double clock = Clock();
    ImGui::Text("%d:%05.2f", static_cast<int>(clock) / 60, std::fmod(clock, 60.0));
    ImGui::SameLine();
    if (ReplayPlaying())
        ImGui::TextColored(Colour(pal::Ok), "replay");
    else if (can)
        ImGui::TextColored(Colour(pal::Muted), "fight");
    else
        ImGui::TextColored(Colour(pal::Muted), "start a replay or Practice to use these");
    if (Holding()) {
        ImGui::SameLine();
        ImGui::TextColored(Colour(pal::Warning), "held");
    }
    if (ImGui::GetTime() < g_noticeUntil) {
        ImGui::SameLine();
        ImGui::TextColored(Colour(pal::Accent), "  %s", g_notice.c_str());
    }
    const char* hint = studio ? "Insert  close the studio" : "Insert  studio      Home  hide this bar";
    ImGui::SameLine(size.x - ImGui::CalcTextSize(hint).x - ImGui::GetStyle().WindowPadding.x);
    ImGui::TextDisabled("%s", hint);
    ImGui::End();
}

void Timeline(ImVec2 position, ImVec2 size)
{
    static double start = 0, end = 30;
    ImGui::SetNextWindowPos(position);
    ImGui::SetNextWindowSize(size);
    ImGui::Begin("##timeline", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollWithMouse);
    std::vector<camera::Key>& keys = camera::Keys();
    const double clock = Clock();
    // The view follows the playhead and always holds every key.
    double last = clock + 5;
    for (const camera::Key& key : keys)
        last = std::max(last, key.Time + 2);
    if (clock > end - 2 || last > end)
        end = std::max(last, clock + 10);
    if (clock < start)
        start = std::max(0.0, clock - 2);
    if (end - start < 10)
        end = start + 10;

    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float font = ImGui::GetFontSize();
    const float left = position.x + font * 7, width = size.x - font * 8;
    const float rulerTop = position.y + font * 0.4f, laneTop = rulerTop + font * 1.4f, laneHeight = font * 1.6f;
    auto toX = [&](double time) { return left + static_cast<float>((time - start) / (end - start)) * width; };
    auto toTime = [&](float x) { return start + static_cast<double>(x - left) / width * (end - start); };

    draw->AddText({ position.x + font * 0.6f, laneTop + (laneHeight - font) * 0.5f }, Hex(pal::Text), "Camera");
    draw->AddRectFilled({ left, laneTop }, { left + width, laneTop + laneHeight }, Hex(pal::Surface));
    const double span = end - start;
    const double tick = span > 120 ? 30 : span > 60 ? 10 : span > 20 ? 5 : 1;
    for (double time = std::ceil(start / tick) * tick; time <= end; time += tick) {
        const float x = toX(time);
        draw->AddLine({ x, rulerTop + font }, { x, laneTop + laneHeight }, Hex(pal::Border));
        char label[16];
        snprintf(label, sizeof(label), "%d:%02d", static_cast<int>(time) / 60, static_cast<int>(time) % 60);
        draw->AddText({ x + 3, rulerTop }, Hex(pal::Muted), label);
    }
    const float mid = laneTop + laneHeight * 0.5f;
    if (keys.size() >= 2)
        draw->AddLine({ toX(keys.front().Time), mid }, { toX(keys.back().Time), mid }, Hex(pal::Muted), 2.0f);
    const ImVec2 mouse = ImGui::GetMousePos();
    const bool inLane = ImGui::IsWindowHovered() && mouse.y >= laneTop && mouse.y < laneTop + laneHeight;
    int hit = -1;
    const float radius = font * 0.45f;
    for (int i = 0; i < static_cast<int>(keys.size()); ++i) {
        const ImVec2 c { toX(keys[i].Time), mid };
        if (std::fabs(mouse.x - c.x) < radius + 2 && inLane)
            hit = i;
        const ImU32 fill = i == g_selectedKey ? IM_COL32_WHITE : Hex(pal::Accent);
        draw->AddQuadFilled({ c.x, c.y - radius }, { c.x + radius, c.y }, { c.x, c.y + radius }, { c.x - radius, c.y }, fill);
    }
    if (hit >= 0) {
        ImGui::SetTooltip("Key %d at %.2f s\nDrag to move it in time, right-click to delete", hit + 1, keys[hit].Time);
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            g_selectedKey = hit;
            g_dragKey = hit;
        }
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            keys.erase(keys.begin() + hit);
            g_selectedKey = -1;
            g_dragKey = -1;
        }
    }
    if (g_dragKey >= 0 && g_dragKey < static_cast<int>(keys.size())) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left) && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 2.0f)) {
            camera::Key moved = keys[g_dragKey];
            moved.Time = std::max(toTime(mouse.x), 0.0);
            keys.erase(keys.begin() + g_dragKey);
            const auto at = std::lower_bound(keys.begin(), keys.end(), moved.Time, [](const camera::Key& k, double t) { return k.Time < t; });
            g_dragKey = static_cast<int>(at - keys.begin());
            keys.insert(at, moved);
            g_selectedKey = g_dragKey;
        } else if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            g_dragKey = -1;
        }
    }
    // The playhead.
    const float x = toX(clock);
    if (x >= left && x <= left + width) {
        draw->AddLine({ x, rulerTop }, { x, laneTop + laneHeight }, IM_COL32_WHITE, 2.0f);
        draw->AddTriangleFilled({ x - 5, rulerTop }, { x + 5, rulerTop }, { x, rulerTop + 7 }, IM_COL32_WHITE);
    }
    ImGui::SetCursorScreenPos({ left, laneTop + laneHeight + font * 0.3f });
    ImGui::TextDisabled("To go to another moment, use BlueSpark's own replay controls: Select + Left / Right for rounds, Select + Down for points to jump to.");
    ImGui::End();
}

void RightPanel(ImVec2 position, ImVec2 size)
{
    ImGui::SetNextWindowPos(position);
    ImGui::SetNextWindowSize(size);
    ImGui::Begin("##panel", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    ImGui::TextColored(Colour(pal::Muted), "Tekken Studio v0.1 beta  ·  TTT2");
    if (ImGui::BeginTabBar("tabs")) {
        if (ImGui::BeginTabItem("Replays")) {
            ReplaysTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Camera")) {
            CameraTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Time")) {
            TimeTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Record")) {
            RecordTab();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
}

// ----- Keys and the free camera

bool Pressed(int vk)
{
    static bool was[256] = {};
    const bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
    const bool pressed = down && !was[vk];
    was[vk] = down;
    return pressed;
}

void FreeCameraInput(ImGuiIO& io)
{
    camera::Settings& s = camera::Config();
    if (s.Mode != camera::Mode::Free || io.WantTextInput)
        return;
    camera::Pose& p = s.Free;
    const float dt = std::clamp(io.DeltaTime, 0.0f, 0.1f);
    float speed = s.FlySpeed * dt;
    if (ImGui::IsKeyDown(ImGuiKey_LeftShift))
        speed *= 4;
    if (ImGui::IsKeyDown(ImGuiKey_LeftCtrl))
        speed *= 0.25f;
    const float yaw = p.Yaw * 3.14159265f / 180.0f, pitch = p.Pitch * 3.14159265f / 180.0f;
    const camera::Vec3 forward { std::sin(yaw) * std::cos(pitch), std::sin(pitch), -std::cos(yaw) * std::cos(pitch) };
    const camera::Vec3 right { std::cos(yaw), 0, std::sin(yaw) };
    auto move = [&](camera::Vec3 d, float amount) {
        p.Eye.X += d.X * amount;
        p.Eye.Y += d.Y * amount;
        p.Eye.Z += d.Z * amount;
    };
    if (ImGui::IsKeyDown(ImGuiKey_W))
        move(forward, speed);
    if (ImGui::IsKeyDown(ImGuiKey_S))
        move(forward, -speed);
    if (ImGui::IsKeyDown(ImGuiKey_D))
        move(right, speed);
    if (ImGui::IsKeyDown(ImGuiKey_A))
        move(right, -speed);
    if (ImGui::IsKeyDown(ImGuiKey_E))
        p.Eye.Y += speed;
    if (ImGui::IsKeyDown(ImGuiKey_Q))
        p.Eye.Y -= speed;
    const bool overPicture = !ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow) && !ImGui::IsAnyItemActive();
    if (ImGui::IsMouseDown(ImGuiMouseButton_Right) && overPicture) {
        const float scale = s.LookSpeed * p.Fov / 50.0f; // a longer lens turns slower
        p.Yaw += io.MouseDelta.x * scale;
        p.Pitch = std::clamp(p.Pitch - io.MouseDelta.y * scale, -89.0f, 89.0f);
    }
    if (io.MouseWheel != 0 && overPicture)
        p.Fov = std::clamp(p.Fov - io.MouseWheel * 2.0f, 5.0f, 130.0f);
}

std::atomic<bool> g_dumpFrame { false };
bool g_hideTheirs = true; // BlueSpark's overlay off while the editor is open

class EditorDialog : public rex::ui::ImGuiDialog {
public:
    explicit EditorDialog(rex::ui::ImGuiDrawer* drawer)
        : rex::ui::ImGuiDialog(drawer)
    {
    }

protected:
    void OnDraw(ImGuiIO& io) override
    {
        // Research: what's in the frame before the studio draws (BlueSpark's own windows and what it put on the shared lists).
        if (g_dumpFrame.exchange(false)) {
            ImGuiContext* context = ImGui::GetCurrentContext();
            for (ImGuiWindow* window : context->Windows)
                if (window->Active || window->WasActive)
                    Log("imgui window \"%s\" at %.0f %.0f size %.0f %.0f active %d vertices %d", window->Name, window->Pos.x, window->Pos.y, window->Size.x,
                        window->Size.y, window->Active ? 1 : 0, window->DrawList ? window->DrawList->VtxBuffer.Size : 0);
            Log("imgui background list %d vertices, foreground list %d", ImGui::GetBackgroundDrawList()->VtxBuffer.Size, ImGui::GetForegroundDrawList()->VtxBuffer.Size);
        }
        DWORD owner = 0;
        GetWindowThreadProcessId(GetForegroundWindow(), &owner);
        const bool front = owner == GetCurrentProcessId(); // the game's window is the one in front
        // A recording: nothing of the studio or of BlueSpark on the picture, and F9 stops it.
        RecorderOnPresent();
        if (RecorderActive()) {
            HideBlueSparkOverlay(true);
            if (front && Pressed(VK_F9))
                RecorderStop();
            return;
        }
        if (front && Pressed(VK_F9) && (CanHold() || ReplayPlaying()))
            RecorderStart();
        if (front) {
            if (Pressed(VK_INSERT))
                g_mode = g_mode == EditorMode::Studio ? EditorMode::Simple : EditorMode::Studio;
            if (Pressed(VK_HOME) && g_mode != EditorMode::Studio)
                g_mode = g_mode == EditorMode::Simple ? EditorMode::Hidden : EditorMode::Simple;
        }
        const bool studio = g_mode == EditorMode::Studio;
        // BlueSpark's own hints and pill would sit on top of the editor: off while it's open over a fight, back when it closes.
        HideBlueSparkOverlay(studio && g_hideTheirs && (CanHold() || ReplayPlaying()));
        // The studio has the keyboard while it's open, so W A S D fly the camera instead of walking a fighter.
        if (studio != g_keysCaptured) {
            g_keysCaptured = studio;
            Ttt2KeyboardCapture(studio);
        }
        if (studio && front && !io.WantTextInput) {
            if (Pressed(VK_F5))
                camera::SetMode(camera::Config().Mode == camera::Mode::Free ? camera::Mode::Game : camera::Mode::Free);
            if (Pressed(VK_F6))
                camera::SetMode(camera::Config().Mode == camera::Mode::Dolly ? camera::Mode::Game : camera::Mode::Dolly);
            if (Pressed('K'))
                camera::AddKeyAtView(Clock());
            if (Pressed(VK_SPACE))
                Time().Paused = !Time().Paused;
            if (Pressed(VK_OEM_PERIOD))
                StepOneFrame();
        }

        TimeTick();

        ImDrawList* background = ImGui::GetBackgroundDrawList();
        const ImVec2 display = io.DisplaySize;
        const bool fight = CanHold() || ReplayPlaying();
        if (g_mode == EditorMode::Hidden || (g_mode == EditorMode::Simple && !fight)) {
            const char* label = "Tekken Studio v0.1 beta";
            background->AddText({ display.x - ImGui::CalcTextSize(label).x - 12, 8 }, IM_COL32(255, 255, 255, 110), label);
            return;
        }

        const ImGuiStyle saved = ImGui::GetStyle();
        ApplyStyle(ImGui::GetStyle());
        ImGui::GetStyle().FontScaleMain = std::clamp(display.y / 720.0f, 1.0f, 2.0f) * 1.15f; // the drawer's font is small; this keeps it readable at any window size
        const float font = ImGui::GetFontSize();
        const float barHeight = ImGui::GetFrameHeight() + ImGui::GetStyle().WindowPadding.y * 2;
        if (studio) {
            FreeCameraInput(io);
            const float timelineHeight = font * 6.0f;
            const float panelWidth = std::floor(std::clamp(display.x * 0.27f, font * 22.0f, font * 34.0f));
            const float bottom = barHeight + timelineHeight;
            // The game draws its picture over the whole window, so world points are placed on the whole window too.
            PathInPicture(background, display, true);
            FighterMarkers(background, display);
            RightPanel({ display.x - panelWidth, 0 }, { panelWidth, display.y - bottom });
            PlaybackBar({ 0, display.y - bottom }, { display.x, barHeight }, true);
            Timeline({ 0, display.y - timelineHeight }, { display.x, timelineHeight });
            const char* hint = camera::Config().Mode == camera::Mode::Free ? "hold right mouse  look      W A S D  fly      K  add a key      Space  pause      click a key's dot  move it"
                               : camera::Config().Mode == camera::Mode::Dolly ? "playing the camera path" : "F5  free camera      F6  dolly path";
            background->AddRectFilled({ 0, display.y - bottom - ImGui::GetFrameHeight() }, { display.x - panelWidth, display.y - bottom }, IM_COL32(0, 0, 0, 110));
            background->AddText({ font * 0.6f, display.y - bottom - ImGui::GetFrameHeight() + (ImGui::GetFrameHeight() - font) * 0.5f }, Hex(pal::Text), hint);
        } else {
            PlaybackBar({ 0, display.y - barHeight }, { display.x, barHeight }, false);
        }
        ImGui::GetStyle() = saved;
    }
};

} // namespace

bool& Markers()
{
    return g_markers;
}

EditorMode& Editor()
{
    return g_mode;
}

void CreateEditor(rex::ui::ImGuiDrawer* drawer)
{
    new EditorDialog(drawer); // the drawer keeps it for the life of the game
}

void DumpFrameOnce()
{
    g_dumpFrame = true;
}

} // namespace studio
