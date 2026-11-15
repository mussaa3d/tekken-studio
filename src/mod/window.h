#pragma once
// The size of the game's picture. A recording can ask for any size, bigger than the monitor included: the game is put in a window of
// that size for as long as the recording runs, and gets its own window back afterwards.
namespace window {

// Any thread. 0, 0 gives the game the window it had.
void SetCaptureSize(int width, int height);

// Windows keeps a window from growing past the desktop unless it's told otherwise. True while a capture size is on.
bool AllowsOversize();

// The picture's size as the game draws it now (the game window's inside), for the menu.
void PictureSize(int& width, int& height);

// Game thread, once per frame.
void OnGameTick();

} // namespace window
