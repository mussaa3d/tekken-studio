#include "mod/ue.h"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <map>
#include <mutex>

#include "mod/log.h"
#include "mod/scan.h"

namespace ue {

using namespace off;

Globals& Engine()
{
    static Globals globals;
    return globals;
}

uint32_t ObjectCount()
{
    return Engine().GObjects ? Field<uint32_t>(Engine().GObjects, ArrayNum) : 0;
}

void* ObjectAt(uint32_t index)
{
    void* array = Engine().GObjects;
    if (!array || index >= Field<uint32_t>(array, ArrayNum) || index / ElementsPerChunk >= Field<uint32_t>(array, ArrayNumChunks))
        return nullptr;
    auto chunks = Field<uint8_t**>(array, ArrayChunks);
    uint8_t* chunk = chunks ? chunks[index / ElementsPerChunk] : nullptr;
    return chunk ? *reinterpret_cast<void**>(chunk + (index % ElementsPerChunk) * ObjectItemSize) : nullptr;
}

bool StillAlive(void* object, uint32_t index)
{
    return object && ObjectAt(index) == object;
}

void KeepAlive(void* object, bool keep)
{
    // FUObjectItem: the object, then its internal flags, where RootSet is bit 30 on UE 5.2.
    constexpr int32_t RootSet = 1 << 30;
    void* array = Engine().GObjects;
    if (!object || !array)
        return;
    const uint32_t index = Field<uint32_t>(object, off::InternalIndex);
    if (ObjectAt(index) != object)
        return;
    auto chunks = Field<uint8_t**>(array, ArrayChunks);
    uint8_t* item = chunks[index / ElementsPerChunk] + (index % ElementsPerChunk) * ObjectItemSize;
    auto& flags = *reinterpret_cast<int32_t*>(item + 8);
    flags = keep ? (flags | RootSet) : (flags & ~RootSet);
}

// While a level loads or unloads, a slot can hold an object whose class pointer is already garbage. Reading that would take
// the game down, so the class chain is walked under an exception guard first.
bool ClassChainIsSound(const void* object)
{
    __try {
        const void* cls = Field<void*>(object, ClassPrivate);
        if (!cls || !(Field<uint64_t>(Field<void*>(cls, ClassPrivate), ClassCastFlags) & CASTCLASS_UClass))
            return false;
        int depth = 0;
        for (; cls; cls = Field<void*>(cls, SuperStruct))
            if (++depth > 64)
                return false;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void ForEachObject(const std::function<bool(void*)>& visitor)
{
    const uint32_t count = ObjectCount();
    for (uint32_t i = 0; i < count; ++i)
        if (void* object = ObjectAt(i); object && ClassChainIsSound(object) && !visitor(object))
            return;
}

// FName::AppendString appends into the caller's FString. A large enough buffer stops it reallocating
// with the engine's allocator, which we could not free.
std::string NameOf(const void* object)
{
    if (!object || !Engine().AppendString)
        return {};
    wchar_t buffer[1024];
    FString out;
    out.Data = buffer;
    out.Max = 1024;
    using AppendFn = void (*)(const void*, FString&);
    reinterpret_cast<AppendFn>(Engine().AppendString)(reinterpret_cast<const uint8_t*>(object) + NamePrivate, out);
    if (out.Data != buffer)
        return {};
    std::string result;
    for (int i = 0; i < out.Num && buffer[i]; ++i)
        result.push_back(static_cast<char>(buffer[i]));
    if (const size_t slash = result.rfind('/'); slash != std::string::npos)
        result.erase(0, slash + 1);
    return result;
}

std::string ClassNameOf(const void* object)
{
    return object ? NameOf(Field<void*>(object, ClassPrivate)) : std::string();
}

bool IsChildOf(const void* cls, const void* base)
{
    for (; cls; cls = Field<void*>(cls, SuperStruct))
        if (cls == base)
            return true;
    return false;
}

bool IsA(const void* object, const void* cls)
{
    return IsChildOf(Field<void*>(object, ClassPrivate), cls);
}

void* FindClass(const std::string& name)
{
    // Remember the slot and FName, not the pointer: blueprint classes unload with their level.
    struct Entry {
        uint32_t Index;
        uint64_t Name;
    };
    static std::mutex mutex;
    static std::map<std::string, Entry> cache;
    std::lock_guard lock(mutex);
    if (const auto it = cache.find(name); it != cache.end()) {
        if (void* object = ObjectAt(it->second.Index); object && Field<uint64_t>(object, NamePrivate) == it->second.Name)
            return object;
        cache.erase(it);
    }

    void* found = nullptr;
    ForEachObject([&](void* object) {
        void* cls = Field<void*>(object, ClassPrivate);
        if (cls && (Field<uint64_t>(cls, ClassCastFlags) & CASTCLASS_UClass) && NameOf(object) == name) {
            found = object;
            return false;
        }
        return true;
    });
    if (found)
        cache[name] = { Field<uint32_t>(found, InternalIndex), Field<uint64_t>(found, NamePrivate) };
    return found;
}

std::vector<void*> AllObjectsOf(const void* cls)
{
    std::vector<void*> result;
    if (!cls)
        return result;
    ForEachObject([&](void* object) {
        if (IsA(object, cls) && !(Field<uint32_t>(object, ObjectFlags) & RF_ClassDefaultObject))
            result.push_back(object);
        return true;
    });
    return result;
}

void* FindFunction(const void* cls, const std::string& name)
{
    for (const void* current = cls; current; current = Field<void*>(current, SuperStruct))
        for (void* field = Field<void*>(current, Children); field; field = Field<void*>(field, FieldNext))
            if ((Field<uint64_t>(Field<void*>(field, ClassPrivate), ClassCastFlags) & CASTCLASS_UFunction) && NameOf(field) == name)
                return field;
    return nullptr;
}

namespace {

// FName::AppendString at an arbitrary address, for FField names.
std::string NameAt(const void* nameAddress)
{
    wchar_t buffer[256];
    FString out;
    out.Data = buffer;
    out.Max = 256;
    using AppendFn = void (*)(const void*, FString&);
    reinterpret_cast<AppendFn>(Engine().AppendString)(nameAddress, out);
    if (out.Data != buffer)
        return {};
    std::string result;
    for (int i = 0; i < out.Num && buffer[i]; ++i)
        result.push_back(static_cast<char>(buffer[i]));
    return result;
}

template <typename Visit>
void ForEachProperty(const void* cls, Visit visit)
{
    constexpr int MaxPerClass = 4096;
    if (!Engine().AppendString || !scan::IsReadable(cls, ChildProperties + 8))
        return;
    int guard = 0;
    for (void* field = Field<void*>(cls, ChildProperties); field && guard < MaxPerClass; ++guard) {
        if (!scan::IsReadable(field, PropertyOffsetInternal + 4))
            return;
        if (!visit(NameAt(reinterpret_cast<const uint8_t*>(field) + FieldName), Field<int>(field, PropertyOffsetInternal)))
            return;
        field = Field<void*>(field, FieldNextProperty);
    }
}

} // namespace

namespace {

int PropertyOffsetUnguarded(const void* cls, const std::string& name)
{
    for (const void* current = cls; current; current = Field<void*>(current, SuperStruct)) {
        int found = -1;
        ForEachProperty(current, [&](const std::string& property, int offset) {
            if (property != name)
                return true;
            found = offset;
            return false;
        });
        if (found >= 0)
            return found;
    }
    return -1;
}

void PropertyNamesUnguarded(const void* cls, std::vector<std::string>& names)
{
    // Each with its type (the FFieldClass's name, the first thing in it) and size, so a function's parameters can be read off the list.
    constexpr uintptr_t FieldClassPrivate = 0x08, ElementSize = 0x3C;
    if (!Engine().AppendString || !scan::IsReadable(cls, ChildProperties + 8))
        return;
    int guard = 0;
    for (void* field = Field<void*>(cls, ChildProperties); field && guard < 4096; ++guard) {
        if (!scan::IsReadable(field, PropertyOffsetInternal + 4))
            return;
        const void* type = Field<void*>(field, FieldClassPrivate);
        char offset[16];
        snprintf(offset, sizeof(offset), "0x%X", Field<int>(field, PropertyOffsetInternal));
        names.push_back(NameAt(reinterpret_cast<const uint8_t*>(field) + FieldName) + " @" + offset + " " + (scan::IsReadable(type, 8) ? NameAt(type) : std::string("?")) + " "
                        + std::to_string(Field<int>(field, ElementSize)));
        field = Field<void*>(field, FieldNextProperty);
    }
}

bool BoolPropertyUnguarded(const void* cls, const std::string& name, int& byteOffset, uint8_t& mask)
{
    bool found = false;
    if (!Engine().AppendString || !scan::IsReadable(cls, ChildProperties + 8))
        return false;
    int guard = 0;
    for (void* field = Field<void*>(cls, ChildProperties); field && guard < 4096 && !found; ++guard) {
        if (!scan::IsReadable(field, BoolFieldMask + 1))
            return false;
        if (NameAt(reinterpret_cast<const uint8_t*>(field) + FieldName) == name) {
            byteOffset = Field<int>(field, PropertyOffsetInternal) + Field<uint8_t>(field, BoolByteOffset);
            mask = Field<uint8_t>(field, BoolFieldMask);
            found = true;
        }
        field = Field<void*>(field, FieldNextProperty);
    }
    return found;
}

} // namespace

// The reflection layout hasn't been checked against this game yet. If it's different, these reads can hit bad memory,
// and that must only switch the feature off, not take the game down.
int PropertyOffset(const void* cls, const std::string& name)
{
    __try {
        return PropertyOffsetUnguarded(cls, name);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

void* FindStruct(const std::string& name)
{
    void* found = nullptr;
    ForEachObject([&](void* object) {
        if (ClassNameOf(object) == "ScriptStruct" && NameOf(object) == name) {
            found = object;
            return false;
        }
        return true;
    });
    return found;
}

bool BoolProperty(const void* cls, const std::string& name, int& byteOffset, uint8_t& mask)
{
    __try {
        return BoolPropertyUnguarded(cls, name, byteOffset, mask);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

FName MakeName(const std::wstring& text)
{
    struct {
        FString In;
        FName ReturnValue;
    } params { FString(text), {} };
    static FunctionRef ref { "KismetStringLibrary", "Conv_StringToName" };
    Call(nullptr, ref, &params);
    return params.ReturnValue;
}

std::string NameText(const FName& name)
{
    return Engine().AppendString ? NameAt(&name) : std::string();
}

std::vector<std::string> BoneNames(void* mesh)
{
    std::vector<std::string> names;
    if (!mesh)
        return names;
    struct {
        int32_t ReturnValue;
    } count { 0 };
    static FunctionRef numBones { "SkinnedMeshComponent", "GetNumBones" };
    if (!Call(mesh, numBones, &count) || count.ReturnValue <= 0 || count.ReturnValue > 4096)
        return names;
    static FunctionRef boneName { "SkinnedMeshComponent", "GetBoneName" };
    for (int i = 0; i < count.ReturnValue; ++i) {
        struct {
            int32_t BoneIndex;
            FName ReturnValue;
        } params { i, {} };
        if (Call(mesh, boneName, &params))
            names.push_back(NameText(params.ReturnValue));
    }
    return names;
}

bool SocketTransform(void* component, FName socket, cine::Vec3& location, cine::Rot& rotation)
{
    struct {
        FName Socket;
        cine::Vec3 ReturnValue;
    } atParams { socket, {} };
    struct {
        FName Socket;
        cine::Rot ReturnValue;
    } rotParams { socket, {} };
    static FunctionRef getLocation { "SceneComponent", "GetSocketLocation" };
    static FunctionRef getRotation { "SceneComponent", "GetSocketRotation" };
    if (!Call(component, getLocation, &atParams) || !Call(component, getRotation, &rotParams))
        return false;
    location = atParams.ReturnValue;
    rotation = rotParams.ReturnValue;
    return true;
}

namespace {

void PropertyNamesGuarded(const void* cls, std::vector<std::string>& names)
{
    __try {
        PropertyNamesUnguarded(cls, names);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

} // namespace

std::vector<std::string> PropertyNames(const void* cls)
{
    std::vector<std::string> names;
    PropertyNamesGuarded(cls, names);
    return names;
}

namespace {

void* Resolve(FunctionRef& ref)
{
    if (ref.Resolved.load(std::memory_order_acquire))
        return ref.Function.load(std::memory_order_relaxed);
    static std::mutex mutex;
    std::lock_guard lock(mutex);
    if (!ref.Resolved.load(std::memory_order_relaxed)) {
        void* cls = FindClass(ref.ClassName);
        void* function = cls ? FindFunction(cls, ref.FunctionName) : nullptr;
        if (!function)
            mod::Log("Function %s::%s not found", ref.ClassName, ref.FunctionName);
        ref.Function.store(function, std::memory_order_relaxed);
        ref.DefaultObject.store(cls ? Field<void*>(cls, ClassDefaultObject) : nullptr, std::memory_order_relaxed);
        ref.Resolved.store(true, std::memory_order_release);
    }
    return ref.Function.load(std::memory_order_relaxed);
}

void ProcessEvent(void* object, void* function, void* params)
{
    using Fn = void (*)(void*, void*, void*);
    auto vtable = *reinterpret_cast<Fn**>(object);
    uint32_t& flags = Field<uint32_t>(function, FunctionFlags);
    const uint32_t saved = flags;
    flags |= FUNC_Native;
    vtable[ProcessEventIndex](object, function, params);
    flags = saved;
}

} // namespace

bool Call(void* object, FunctionRef& ref, void* params)
{
    void* function = Resolve(ref);
    if (!function)
        return false;
    if (!object) {
        object = ref.DefaultObject.load(std::memory_order_relaxed);
        if (!object)
            return false;
    } else if (!scan::IsReadable(object, 0x28)) {
        return false;
    }
    ProcessEvent(object, function, params);
    return true;
}

bool CallOwn(void* object, const std::string& name, void* params)
{
    if (!object || !scan::IsReadable(object, 0x28))
        return false;
    void* function = FindFunction(Field<void*>(object, ClassPrivate), name);
    if (!function)
        return false;
    ProcessEvent(object, function, params);
    return true;
}

void* GameViewport()
{
    void* engine = Engine().GEngine ? *Engine().GEngine : nullptr;
    return engine ? Field<void*>(engine, EngineGameViewport) : nullptr;
}

void* World()
{
    void* viewport = GameViewport();
    return viewport ? Field<void*>(viewport, ViewportWorld) : nullptr;
}

void* PlayerController()
{
    struct {
        void* World;
        int32_t Index;
        void* ReturnValue;
    } params { World(), 0, nullptr };
    static FunctionRef ref { "GameplayStatics", "GetPlayerController" };
    Call(nullptr, ref, &params);
    return params.ReturnValue;
}

bool IsGamePaused()
{
    struct {
        void* World;
        bool ReturnValue;
    } params { World(), false };
    static FunctionRef ref { "GameplayStatics", "IsGamePaused" };
    Call(nullptr, ref, &params);
    return params.ReturnValue;
}

void ConsoleCommand(const std::wstring& command)
{
    struct {
        void* World;
        FString Command;
        void* Player;
    } params { World(), FString(command), PlayerController() };
    static FunctionRef ref { "KismetSystemLibrary", "ExecuteConsoleCommand" };
    Call(nullptr, ref, &params);
}

int CvarInt(const std::wstring& name)
{
    struct {
        FString Name;
        int ReturnValue;
    } params { FString(name), 0 };
    static FunctionRef ref { "KismetSystemLibrary", "GetConsoleVariableIntValue" };
    Call(nullptr, ref, &params);
    return params.ReturnValue;
}

void SetGlobalTimeDilation(float dilation)
{
    struct {
        void* World;
        float Dilation;
    } params { World(), dilation };
    static FunctionRef ref { "GameplayStatics", "SetGlobalTimeDilation" };
    if (params.World)
        Call(nullptr, ref, &params);
}

double WorldRealTime()
{
    struct {
        void* World;
        double ReturnValue;
    } params { World(), 0 };
    static FunctionRef ref { "GameplayStatics", "GetRealTimeSeconds" };
    if (params.World)
        Call(nullptr, ref, &params);
    return params.ReturnValue;
}

bool ActorLocation(void* actor, cine::Vec3& out)
{
    struct {
        cine::Vec3 ReturnValue;
    } params;
    static FunctionRef ref { "Actor", "K2_GetActorLocation" };
    if (!Call(actor, ref, &params))
        return false;
    out = params.ReturnValue;
    return true;
}

void* CharacterMesh(void* character)
{
    struct {
        void* ReturnValue;
    } params { nullptr };
    static FunctionRef ref { "PolarisCharacterActor", "GetMeshComponent" };
    Call(character, ref, &params);
    return params.ReturnValue;
}

void DestroyActor(void* actor)
{
    static FunctionRef ref { "Actor", "K2_DestroyActor" };
    Call(actor, ref, nullptr);
}

namespace {

// Identity transform: rotation quaternion, translation, scale, four doubles each.
void WriteIdentity(uint8_t* at)
{
    auto* transform = reinterpret_cast<double*>(at);
    transform[3] = 1.0;
    transform[8] = transform[9] = transform[10] = 1.0;
}

// An object's own name, with nothing cut off: a package's name is its whole path.
std::string RawNameOf(const void* object)
{
    return object && Engine().AppendString ? NameAt(reinterpret_cast<const uint8_t*>(object) + NamePrivate) : std::string();
}

constexpr uintptr_t OuterPrivate = 0x20;

} // namespace

void* SpawnActor(const std::string& className)
{
    void* library = FindClass("GameplayStatics");
    void* cls = FindClass(className);
    void* begin = library ? FindFunction(library, "BeginDeferredActorSpawnFromClass") : nullptr;
    void* finish = library ? FindFunction(library, "FinishSpawningActor") : nullptr;
    void* world = World();
    if (!cls || !begin || !finish || !world)
        return nullptr;
    const int worldAt = PropertyOffset(begin, "WorldContextObject");
    const int classAt = PropertyOffset(begin, "ActorClass");
    const int transformAt = PropertyOffset(begin, "SpawnTransform");
    const int collisionAt = PropertyOffset(begin, "CollisionHandlingOverride");
    const int returnAt = PropertyOffset(begin, "ReturnValue");
    const int finishActorAt = PropertyOffset(finish, "Actor");
    const int finishTransformAt = PropertyOffset(finish, "SpawnTransform");
    if (worldAt < 0 || classAt < 0 || transformAt < 0 || returnAt < 0 || finishActorAt < 0 || finishTransformAt < 0) {
        static bool warned = false;
        if (!warned)
            mod::Log("Spawn: the spawn functions have an unexpected layout");
        warned = true;
        return nullptr;
    }
    alignas(16) uint8_t params[0x200] = {};
    *reinterpret_cast<void**>(params + worldAt) = world;
    *reinterpret_cast<void**>(params + classAt) = cls;
    WriteIdentity(params + transformAt);
    if (collisionAt >= 0)
        params[collisionAt] = 1; // always spawn
    static FunctionRef beginRef { "GameplayStatics", "BeginDeferredActorSpawnFromClass" };
    Call(nullptr, beginRef, params);
    void* actor = *reinterpret_cast<void**>(params + returnAt);
    if (!actor)
        return nullptr;

    alignas(16) uint8_t finishParams[0x200] = {};
    *reinterpret_cast<void**>(finishParams + finishActorAt) = actor;
    WriteIdentity(finishParams + finishTransformAt);
    static FunctionRef finishRef { "GameplayStatics", "FinishSpawningActor" };
    Call(nullptr, finishRef, finishParams);
    return actor;
}

void SetActorPose(void* actor, cine::Vec3 location, cine::Rot rotation)
{
    static void* cls = FindClass("Actor");
    static void* function = cls ? FindFunction(cls, "K2_SetActorLocationAndRotation") : nullptr;
    static const int rotationAt = function ? PropertyOffset(function, "NewRotation") : -1;
    static const int teleportAt = function ? PropertyOffset(function, "bTeleport") : -1;
    if (rotationAt < 0 || teleportAt < 0 || teleportAt > 0x3F0)
        return;
    alignas(16) uint8_t params[0x400] = {}; // the function also takes a hit result to fill in
    *reinterpret_cast<cine::Vec3*>(params) = location;
    *reinterpret_cast<cine::Rot*>(params + rotationAt) = rotation;
    params[teleportAt] = 1;
    static FunctionRef ref { "Actor", "K2_SetActorLocationAndRotation" };
    Call(actor, ref, params);
}

void SetActorScale(void* actor, cine::Vec3 scale)
{
    static FunctionRef ref { "Actor", "SetActorScale3D" };
    Call(actor, ref, &scale);
}

void SetActorHidden(void* actor, bool hidden)
{
    struct {
        bool Hidden;
    } params { hidden };
    static FunctionRef ref { "Actor", "SetActorHiddenInGame" };
    Call(actor, ref, &params);
}

void* ComponentOwner(void* component)
{
    struct {
        void* ReturnValue;
    } params { nullptr };
    static FunctionRef ref { "ActorComponent", "GetOwner" };
    Call(component, ref, &params);
    return params.ReturnValue;
}

bool ComponentTransform(void* component, cine::Xform& out)
{
    struct {
        double Rotation[4], Translation[4], Scale[4];
    } params {};
    params.Rotation[3] = 1;
    static FunctionRef ref { "SceneComponent", "K2_GetComponentToWorld" };
    if (!Call(component, ref, &params))
        return false;
    out.Rotation = { params.Rotation[0], params.Rotation[1], params.Rotation[2], params.Rotation[3] };
    out.Translation = { params.Translation[0], params.Translation[1], params.Translation[2] };
    out.Scale = { params.Scale[0], params.Scale[1], params.Scale[2] };
    return true;
}

void SetComponentPose(void* component, cine::Vec3 location, cine::Rot rotation)
{
    static void* cls = FindClass("SceneComponent");
    static void* function = cls ? FindFunction(cls, "K2_SetWorldLocationAndRotation") : nullptr;
    static const int rotationAt = function ? PropertyOffset(function, "NewRotation") : -1;
    static const int teleportAt = function ? PropertyOffset(function, "bTeleport") : -1;
    if (rotationAt < 0 || teleportAt < 0 || teleportAt > 0x3F0)
        return;
    alignas(16) uint8_t params[0x400] = {};
    *reinterpret_cast<cine::Vec3*>(params) = location;
    *reinterpret_cast<cine::Rot*>(params + rotationAt) = rotation;
    params[teleportAt] = 1;
    static FunctionRef ref { "SceneComponent", "K2_SetWorldLocationAndRotation" };
    Call(component, ref, params);
}

void SetComponentScale(void* component, cine::Vec3 scale)
{
    static FunctionRef ref { "SceneComponent", "SetWorldScale3D" };
    Call(component, ref, &scale);
}

void SetMovable(void* component)
{
    static void* scene = FindClass("SceneComponent");
    static const int at = scene ? PropertyOffset(scene, "Mobility") : -1;
    if (at >= 0 && Field<uint8_t>(component, at) == 2)
        return;
    uint8_t movable = 2;
    static FunctionRef ref { "SceneComponent", "SetMobility" };
    Call(component, ref, &movable);
}

std::string PathOf(void* object)
{
    if (!object || !scan::IsReadable(object, 0x28))
        return {};
    std::string path = RawNameOf(object);
    int depth = 0;
    for (void* outer = Field<void*>(object, OuterPrivate); outer && scan::IsReadable(outer, 0x28) && depth < 8; outer = Field<void*>(outer, OuterPrivate), ++depth) {
        // The outermost one is the package, whose name is the path on disk. Anything between is a subobject.
        const bool package = Field<void*>(outer, OuterPrivate) == nullptr;
        path = RawNameOf(outer) + (package ? "." : ":") + path;
    }
    return path;
}

void* FindLoaded(const std::string& className, const std::string& nameOrPath)
{
    void* cls = FindClass(className);
    if (!cls || nameOrPath.empty())
        return nullptr;
    const bool byPath = nameOrPath.find('/') != std::string::npos;
    const std::string shortName = byPath ? nameOrPath.substr(nameOrPath.find_last_of("/.") + 1) : nameOrPath;
    void* found = nullptr;
    ForEachObject([&](void* object) {
        if (!IsA(object, cls) || (Field<uint32_t>(object, ObjectFlags) & RF_ClassDefaultObject) || RawNameOf(object) != shortName)
            return true;
        if (byPath && PathOf(object) != nameOrPath)
            return true;
        found = object;
        return false;
    });
    return found;
}

void* LoadAsset(const std::string& path)
{
    void* library = FindClass("KismetSystemLibrary");
    void* make = library ? FindFunction(library, "MakeSoftObjectPath") : nullptr;
    void* convert = library ? FindFunction(library, "Conv_SoftObjPathToSoftObjRef") : nullptr;
    void* load = library ? FindFunction(library, "LoadAsset_Blocking") : nullptr;
    if (!make || !convert || !load)
        return nullptr;
    const int makeReturn = PropertyOffset(make, "ReturnValue"), convertIn = PropertyOffset(convert, "SoftObjectPath"), convertReturn = PropertyOffset(convert, "ReturnValue");
    const int loadIn = PropertyOffset(load, "Asset"), loadReturn = PropertyOffset(load, "ReturnValue");
    const int makeSize = Field<uint16_t>(make, FunctionParamsSize), convertSize = Field<uint16_t>(convert, FunctionParamsSize), loadSize = Field<uint16_t>(load, FunctionParamsSize);
    const int pathBytes = makeSize - makeReturn, pointerBytes = convertSize - convertReturn;
    // The sizes come from the functions themselves. If they don't add up, this build lays them out differently, and nothing is called.
    if (makeReturn < 16 || convertIn != 0 || convertReturn < pathBytes || loadIn != 0 || loadReturn < pointerBytes || pathBytes < 16 || pathBytes > 0x60 || pointerBytes < 16
        || pointerBytes > 0x80 || makeSize > 0xE0 || convertSize > 0xE0 || loadSize > 0xE0) {
        mod::Log("Load: the engine's load functions have an unexpected layout (%d %d %d %d %d, sizes %d %d %d)", makeReturn, convertIn, convertReturn, loadIn, loadReturn, makeSize,
                 convertSize, loadSize);
        return nullptr;
    }
    const std::wstring wide(path.begin(), path.end());
    alignas(16) uint8_t makeParams[0x100] = {}, convertParams[0x100] = {}, loadParams[0x100] = {};
    *reinterpret_cast<FString*>(makeParams) = FString(wide);
    static FunctionRef makeRef { "KismetSystemLibrary", "MakeSoftObjectPath" };
    static FunctionRef convertRef { "KismetSystemLibrary", "Conv_SoftObjPathToSoftObjRef" };
    static FunctionRef loadRef { "KismetSystemLibrary", "LoadAsset_Blocking" };
    if (!Call(nullptr, makeRef, makeParams))
        return nullptr;
    std::memcpy(convertParams + convertIn, makeParams + makeReturn, pathBytes);
    if (!Call(nullptr, convertRef, convertParams))
        return nullptr;
    std::memcpy(loadParams + loadIn, convertParams + convertReturn, pointerBytes);
    if (!Call(nullptr, loadRef, loadParams))
        return nullptr;
    return *reinterpret_cast<void**>(loadParams + loadReturn);
}

std::vector<std::string> LoadedNames(const std::string& className)
{
    std::vector<std::string> names;
    void* cls = FindClass(className);
    if (!cls)
        return names;
    ForEachObject([&](void* object) {
        if (IsA(object, cls) && !(Field<uint32_t>(object, ObjectFlags) & RF_ClassDefaultObject))
            names.push_back(RawNameOf(object));
        return true;
    });
    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());
    return names;
}

SlateVisibility WidgetVisibility(void* widget)
{
    struct {
        SlateVisibility ReturnValue;
    } params { SlateVisibility::Visible };
    static FunctionRef ref { "Widget", "GetVisibility" };
    Call(widget, ref, &params);
    return params.ReturnValue;
}

void SetWidgetVisibility(void* widget, SlateVisibility visibility)
{
    struct {
        SlateVisibility Value;
    } params { visibility };
    static FunctionRef ref { "Widget", "SetVisibility" };
    Call(widget, ref, &params);
}

} // namespace ue
