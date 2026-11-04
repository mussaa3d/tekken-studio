#pragma once
// Just enough of the Unreal Engine 5 object model to find objects and call their functions.
// The offsets and the method follow the Tekken Overlay rebuild's notes
// (github.com/parsaes80/tekken-overlay-rebuild), checked against game build 1787726436.
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "common/cinemath.h"

namespace ue {

namespace off {
constexpr uintptr_t ObjectFlags = 0x08;
constexpr uintptr_t InternalIndex = 0x0C;
constexpr uintptr_t ClassPrivate = 0x10;
constexpr uintptr_t NamePrivate = 0x18;
constexpr uintptr_t SuperStruct = 0x40;
constexpr uintptr_t Children = 0x48;
constexpr uintptr_t FieldNext = 0x28;
constexpr uintptr_t FunctionFlags = 0xB0;
constexpr uintptr_t ClassCastFlags = 0xD8;
constexpr uintptr_t ClassDefaultObject = 0x110;
constexpr uintptr_t ArrayChunks = 0x00;
constexpr uintptr_t ArrayNum = 0x14;
constexpr uintptr_t ArrayNumChunks = 0x1C;
constexpr size_t ObjectItemSize = 0x18;
constexpr uint32_t ElementsPerChunk = 0x10000;
constexpr uintptr_t EngineGameViewport = 0x9B8;
constexpr uintptr_t EngineFrameFlags = 0x9DC; // bit 0x40 = bSmoothFrameRate
constexpr uintptr_t EngineFixedFrameRate = 0x9E0;
constexpr uintptr_t ViewportWorld = 0x78;
constexpr uintptr_t ViewportShowFlags = 0xB8; // FEngineShowFlags: a bitfield read straight from memory
constexpr uintptr_t PovLocation = 0x22D0; // APlayerCameraManager.CameraCachePrivate.POV
constexpr uintptr_t PovRotation = 0x22E8;
constexpr uintptr_t PovFov = 0x2300;
constexpr uintptr_t ViewInfoFov = 0x30; // FMinimalViewInfo.FOV, in the view given to the projection function
constexpr uintptr_t CharacterSide = 0x7AC; // PolarisCharacterActor: 0 = P1, 1 = P2
constexpr uintptr_t MeshSpaceTransforms = 0x950;
constexpr int ProcessEventIndex = 77;
constexpr uint32_t FUNC_Native = 0x400;
constexpr uint64_t CASTCLASS_UClass = 0x20;
constexpr uint64_t CASTCLASS_UFunction = 0x80000;
constexpr uint32_t RF_ClassDefaultObject = 0x10;
// FField, the UE5 reflection records. Standard for UE 5.3; every read is range-checked because these aren't
// from the Tekken Overlay notes and haven't been checked against this game yet.
constexpr uintptr_t ChildProperties = 0x50;
constexpr uintptr_t FieldNextProperty = 0x20;
constexpr uintptr_t FieldName = 0x28;
constexpr uintptr_t PropertyOffsetInternal = 0x4C;
constexpr uintptr_t BoolByteOffset = 0x79;
constexpr uintptr_t BoolFieldMask = 0x7B;
} // namespace off

template <typename T>
struct TArray {
    T* Data = nullptr;
    int32_t Num = 0;
    int32_t Max = 0;
};

struct FString : TArray<wchar_t> {
    FString() = default;
    explicit FString(const std::wstring& text)
    {
        Data = const_cast<wchar_t*>(text.c_str());
        Num = Max = static_cast<int32_t>(text.size() + 1);
    }
};

template <typename T>
T& Field(const void* object, uintptr_t offset)
{
    return *reinterpret_cast<T*>(reinterpret_cast<uintptr_t>(object) + offset);
}

struct Globals {
    void* GObjects = nullptr;
    void** GEngine = nullptr;
    void* AppendString = nullptr;
};
Globals& Engine();

uint32_t ObjectCount();
void* ObjectAt(uint32_t index);
bool StillAlive(void* object, uint32_t index);
std::string NameOf(const void* object);
std::string ClassNameOf(const void* object);
bool IsA(const void* object, const void* cls);
bool IsChildOf(const void* cls, const void* base);
void* FindClass(const std::string& name);
std::vector<void*> AllObjectsOf(const void* cls);
void* FindFunction(const void* cls, const std::string& name);
bool ClassChainIsSound(const void* object);
void ForEachObject(const std::function<bool(void*)>& visitor);

// Byte offset of a reflected property (FProperty), walking up the parents. -1 if not found. Lets the mod read
// a value before overriding it, so it can be put back.
int PropertyOffset(const void* cls, const std::string& name);
// Names of a class's own properties, for the class dump.
std::vector<std::string> PropertyNames(const void* cls);

// A function looked up once per call site, then called through UObject::ProcessEvent.
struct FunctionRef {
    const char* ClassName;
    const char* FunctionName;
    std::atomic<bool> Resolved { false };
    std::atomic<void*> Function { nullptr };
    std::atomic<void*> DefaultObject { nullptr };
};
bool Call(void* object, FunctionRef& function, void* params);

// A reflected struct such as "PostProcessSettings", and a bool property inside one (bools can be single bits).
void* FindStruct(const std::string& name);
bool BoolProperty(const void* cls, const std::string& name, int& byteOffset, uint8_t& mask);

// FName by value, as engine functions take it.
struct FName {
    int32_t Index = 0;
    int32_t Number = 0;
};
FName MakeName(const std::wstring& text);
std::string NameText(const FName& name);

std::vector<std::string> BoneNames(void* skinnedMesh);
bool SocketTransform(void* component, FName socket, cine::Vec3& location, cine::Rot& rotation);

void* GameViewport();
void* World();
void* PlayerController();
bool IsGamePaused();
void ConsoleCommand(const std::wstring& command);
int CvarInt(const std::wstring& name);
void SetGlobalTimeDilation(float dilation);
bool ActorLocation(void* actor, cine::Vec3& out);
void* CharacterMesh(void* character);
void DestroyActor(void* actor);

enum class SlateVisibility : uint8_t { Visible, Collapsed, Hidden, HitTestInvisible, SelfHitTestInvisible };
SlateVisibility WidgetVisibility(void* widget);
void SetWidgetVisibility(void* widget, SlateVisibility visibility);

} // namespace ue
