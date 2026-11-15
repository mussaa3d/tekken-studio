#pragma once
// Hooks the D3D12 swap chain so the editor can be drawn over the game and frames can be captured.
#include <cstdint>
#include <filesystem>
#include <string>

struct HWND__;

namespace overlay {

bool Install();

bool MenuOpen();
void ToggleMenu();
bool IsGameForeground();

// The game's window, once its first frame has been shown, and whether it has the whole screen to itself.
HWND__* GameWindow();
bool ExclusiveFullscreen();

// Whether a key or mouse button is held: from the window's own messages, and from Windows where it answers.
bool KeyDown(int virtualKey);

// A text box has the keyboard, so typed letters mustn't fly the camera or trigger hotkeys.
bool KeyboardTakenByUi();

// The editor tells the camera where the game picture is on screen, so right mouse and the wheel only act on the camera
// when the cursor is over it.
void SetGameViewHovered(bool hovered);
bool MouseOverGameView();

// Mouse wheel ticks since the last call (the free camera's speed control).
int TakeMouseWheel();

// Makes sure the editor's font can show this text, such as a player's name in any script.
void AddFontCharacters(const std::string& text);

// Test only: writes the next frame, as the game drew it and before the editor goes on it, to a file of raw pixels, and logs its size.
void RequestScreenshot(const std::filesystem::path& path);

// The game's picture as a texture for the editor to draw, and its width over height. 0 before the first frame.
uint64_t GameTexture();
float GameAspect();

// Game thread, once per tick: hotkeys.
void OnGameTick();

// Test harness only: open the editor, capture pictures of each tab, record, and play a test tone.
struct HarnessOptions {
    bool Enabled = false;
    int RecordFrames = 0;
    bool Screenshots = false;
    bool UiTest = false;
    bool Preview = false; // run the real tick and camera code on a fake scene
    wchar_t ScreenshotBase[260] = L"";
};
void SetHarness(const HarnessOptions& options);
bool HarnessBusy();

} // namespace overlay

namespace menu {
void ApplyStyle(float fontSize);
void Draw();
void DrawOsd();
void ShowNotice(const char* text);
// A message over the game for a few seconds, shown with the editor closed too. Any thread.
void Flash(const std::string& text);
void SetTab(int tab);
void OpenAllGroups(bool open); // test only: every section of a tab drawn open
constexpr int TabCount = 5;
bool RunUiTest(); // true once it has finished
} // namespace menu
