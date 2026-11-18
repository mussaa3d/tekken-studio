#pragma once
// The game's HUD and the picture's shape. Tekken 8 draws each HUD screen (the gauges, the replay and practice controls, the round
// calls, the control guide) as a full-window canvas holding a ScaleBox that fits a 3840 x 2160 design into it (measured). So:
//   InPicture: each of those screens is anchored to the part of the window the picture is in, inside the bars a fixed shape adds,
//              and the HUD fits the picture the way it fits a recording of that shape.
//   Shape:     the same, and the 3840 x 2160 boxes in each design take the picture's shape too: in portrait the gauges go to the top
//              of the picture and the controls to the bottom, in a wide shape the side parts go to the sides.
// Game thread.
#include <string>

namespace hudframe {

enum class Mode { Game, InPicture, Shape };

// Each game tick. `aspect` is the picture's shape (width over height), 0 for the game's own; `windowWidth` x `windowHeight` the
// window the game draws in.
void Apply(Mode mode, float aspect, int windowWidth, int windowHeight);

// Research: logs the HUD widgets on screen as trees (only classes whose name holds `words`, all if empty), and anchors every widget
// on screen to a part of the window.
void Dump(const std::string& words);
void Anchor(float minX, float minY, float maxX, float maxY);

} // namespace hudframe
