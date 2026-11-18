// The game's camera. TTT2 keeps up to twelve camera records in a manager and marks one as the one in use. A record holds where the
// camera is, what it looks at and its lens:
//
//   [0x82E6B05C] -> the game system; +724 -> the camera object; +436 -> the manager; +316 = the slot in use;
//   [manager + 340 + slot * 4] -> the slot's block; +16 = the record: eye xyz (+0), target xyz (+16), field of view in degrees (+56)
//
// Read from the game's own getters (sub_82565678, sub_825468D0, sub_82542618, sub_825426C0, sub_825427C8), on BlueSpark release 23.
#include "camera.h"

#include "studio.h"

#include <rex/ppc/context.h>

namespace studio::camera {

namespace {
constexpr uint32_t GameSystemHolder = 0x82E6B05C;
constexpr uint32_t CameraObject = 724, Manager = 436, SlotInUse = 316, Slots = 340, SlotCount = 12, RecordInBlock = 16;
} // namespace

uint32_t Record()
{
    if (!Readable(GameSystemHolder, 4))
        return 0;
    const uint32_t system = ReadU32(GameSystemHolder);
    if (!system || !Readable(system + CameraObject, 4))
        return 0;
    const uint32_t object = ReadU32(system + CameraObject);
    if (!object || !Readable(object + Manager, 4))
        return 0;
    const uint32_t manager = ReadU32(object + Manager);
    if (!manager || !Readable(manager + SlotInUse, 4))
        return 0;
    const uint32_t slot = ReadU32(manager + SlotInUse);
    if (slot >= SlotCount || !Readable(manager + Slots + slot * 4, 4))
        return 0;
    const uint32_t block = ReadU32(manager + Slots + slot * 4);
    if (!block || !Readable(block + RecordInBlock, 64))
        return 0;
    return block + RecordInBlock;
}

bool Read(View& out)
{
    const uint32_t record = Record();
    if (!record)
        return false;
    out.Eye = { ReadFloat(record + 0), ReadFloat(record + 4), ReadFloat(record + 8) };
    out.Target = { ReadFloat(record + 16), ReadFloat(record + 20), ReadFloat(record + 24) };
    out.Fov = ReadFloat(record + 56);
    return true;
}

bool& Override()
{
    static bool on = false;
    return on;
}

View& Studio()
{
    static View view;
    return view;
}

void Apply()
{
    const uint32_t record = Record();
    if (!record || !Override())
        return;
    const View& v = Studio();
    WriteFloat(record + 0, v.Eye.X);
    WriteFloat(record + 4, v.Eye.Y);
    WriteFloat(record + 8, v.Eye.Z);
    WriteFloat(record + 16, v.Target.X);
    WriteFloat(record + 20, v.Target.Y);
    WriteFloat(record + 24, v.Target.Z);
    WriteFloat(record + 56, v.Fov);
}

} // namespace studio::camera

// The game's function that writes the frame's camera into its record, once a frame. BlueSpark doesn't hook it (checked against
// ttt2_ours.lib of release 23). The original runs first, then the studio's view goes over what it wrote.
extern "C" void __imp__sub_82541878(PPCContext& ctx, uint8_t* base);
extern "C" void sub_82541878(PPCContext& ctx, uint8_t* base)
{
    __imp__sub_82541878(ctx, base);
    studio::camera::Apply();
}

