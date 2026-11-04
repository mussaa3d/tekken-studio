#pragma once
// Depth of field and colour, done with the post-process settings the engine reads from the camera each frame. This
// is what IWXMVM's Filmtweaks and DOF sections do, on this engine.
#include <string>
#include <vector>

#include "common/cinemath.h"

namespace postfx {

enum class Type { Float, AllChannels };

struct Control {
    const char* Group;
    const char* Label;
    const char* Property;
    Type Type = Type::Float;
    float Min = 0, Max = 1, Default = 0;
    float Value = 0;
    bool On = false;
    int ValueOffset = -1;
    int OverrideByte = -1;
    unsigned char OverrideMask = 0;
};

std::vector<Control>& All();

enum class Focus { Manual, P1, P2, BetweenFighters };
struct FocusSettings {
    Focus Mode = Focus::Manual;
};
FocusSettings& FocusConfig();

// Game thread, each frame, after the camera is final.
void Apply(void* cameraManager, cine::Vec3 cameraLocation);

// The settings are looked for the first time a control is used. Checked says whether that's happened yet, and Available
// whether they were found.
bool Checked();
bool Available();

} // namespace postfx
