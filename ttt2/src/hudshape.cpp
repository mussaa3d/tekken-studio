// The game's HUD fitted to the picture's shape. Measured on release 23, on the game's render thread:
//   - sub_827366A0 is its "set viewport": the device, then X, Y, width, height and the depth range as floats. A frame sets a full
//     1280 x 720 one about a dozen times, for its passes.
//   - sub_82732970 sets shader constants: the device, the first register, the values, how many vectors. Once a frame it hands
//     over a projection for a 1280 x 720 screen ({ 2/1280 0 0 0 | 0 -2/720 0 0 | 0 0 z 0 | -1 1 zoff 1 }) on register 4, then 8:
//     that's the start of the HUD's pass (the health bars, timer, portraits and names are drawn next, in screen pixels through
//     the viewport). The same projection goes to register 128 twice a frame for the mode's label ("VS BATTLE"), which is drawn
//     through the projection itself.
// Shrinking the viewport that came just before that register-4 projection shrank the whole HUD and nothing else. So when the
// projection for register 4 goes by, the studio sets the viewport to where the HUD belongs in the shape (the game sets its own
// again for its next pass), and the register-128 projection is scaled and moved the same way for the label. The values the game
// keeps are put back right after each call, so nothing builds up.
#include "studio.h"

#include "camera.h"

#include <algorithm>
#include <atomic>

#include <rex/ppc/context.h>

extern "C" void __imp__sub_827366A0(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_82732970(PPCContext& ctx, uint8_t* base);

namespace studio {

namespace {
constexpr float ScreenWidth = 1280, ScreenHeight = 720; // the game's own picture, whatever the window
std::atomic<uint64_t> g_passes { 0 };
bool g_follow = true;

// Where the HUD goes: the whole 16:9 HUD, as big as fits the shape, along the top of it and centred. False with no shape.
bool HudRect(float& x, float& y, float& width, float& height)
{
    if (!g_follow || !(camera::Config().ShapeAspect > 0))
        return false;
    float left = 0, top = 0, right = 0, bottom = 0;
    camera::ShapeRect(ScreenWidth, ScreenHeight, left, top, right, bottom);
    const float fit = std::min((right - left) / ScreenWidth, (bottom - top) / ScreenHeight);
    if (fit > 0.999f)
        return false;
    // Bigger than fits, up to the game's own size: the middle of the HUD stays in the shape and its ends run off the sides.
    const float scale = std::min(fit * std::clamp(camera::Config().HudSize, 1.0f, 4.0f), 1.0f);
    width = ScreenWidth * scale;
    height = ScreenHeight * scale;
    x = left + ((right - left) - width) * 0.5f;
    y = top;
    return true;
}
} // namespace

bool& HudFollowsShape()
{
    return g_follow;
}

uint64_t HudPassesSeen()
{
    return g_passes;
}

} // namespace studio

extern "C" void sub_82732970(PPCContext& ctx, uint8_t* base)
{
    using namespace studio;
    const uint32_t first = ctx.r4.u32, data = ctx.r5.u32, count = ctx.r6.u32;
    const bool projection = count == 4 && (first == 4 || first == 128) && Readable(data, 64) && ReadFloat(data) == 0.0015625f && ReadFloat(data + 20) == -0.0027777778f
                            && ReadFloat(data + 48) == -1.0f && ReadFloat(data + 52) == 1.0f;
    float x = 0, y = 0, width = 0, height = 0;
    if (!projection || !HudRect(x, y, width, height)) {
        __imp__sub_82732970(ctx, base);
        return;
    }
    if (first == 4) {
        // The HUD's pass starts here: its viewport, set through the game's own function on a copy of the registers.
        ++g_passes;
        PPCContext call = ctx;
        call.f1.f64 = x;
        call.f2.f64 = y;
        call.f3.f64 = width;
        call.f4.f64 = height;
        call.f5.f64 = 0.0;
        call.f6.f64 = 1.0;
        __imp__sub_827366A0(call, base);
        __imp__sub_82732970(ctx, base);
        return;
    }
    // The label's projection: the same place, in the screen's own -1 to 1 units (y up).
    const float sx = width / ScreenWidth, sy = height / ScreenHeight;
    const float mx = (x + width * 0.5f - ScreenWidth * 0.5f) / (ScreenWidth * 0.5f), my = -(y + height * 0.5f - ScreenHeight * 0.5f) / (ScreenHeight * 0.5f);
    WriteFloat(data, 0.0015625f * sx);
    WriteFloat(data + 20, -0.0027777778f * sy);
    WriteFloat(data + 48, -sx + mx);
    WriteFloat(data + 52, sy + my);
    __imp__sub_82732970(ctx, base);
    WriteFloat(data, 0.0015625f);
    WriteFloat(data + 20, -0.0027777778f);
    WriteFloat(data + 48, -1.0f);
    WriteFloat(data + 52, 1.0f);
}
