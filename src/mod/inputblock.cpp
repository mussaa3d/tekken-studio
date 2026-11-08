#include "mod/inputblock.h"

#include <windows.h>

#include <initguid.h>
#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>

#include <MinHook.h>

#include <atomic>
#include <cstring>

#include "mod/guard.h"
#include "mod/log.h"
#include "mod/overlay.h"
#include "mod/scan.h"

namespace inputblock {

namespace {

// The window procedure stops key messages, but the game also reads held keys by itself, which is how A and D kept rewinding the replay
// with the editor open. The game's own calls get "nothing is pressed" while the editor is open. The mod's and the overlay's calls go through.
uintptr_t g_gameBegin = 0, g_gameEnd = 0;

bool Blocking()
{
    return overlay::MenuOpen() && !guard::Tripped();
}

void Note(std::atomic<bool>& said, const char* source)
{
    if (!said.exchange(true))
        mod::Log("Input: the game reads keys through %s, held back while the editor is open", source);
}

using KeyStateFn = SHORT(WINAPI*)(int);
KeyStateFn oAsync = nullptr, oKey = nullptr;
std::atomic<bool> g_saidAsync { false }, g_saidKey { false }, g_saidState { false }, g_saidData { false };

SHORT WINAPI DetourAsync(int key)
{
    if (Blocking()) {
        Note(g_saidAsync, "GetAsyncKeyState");
        return 0;
    }
    return oAsync(key);
}

SHORT WINAPI DetourKey(int key)
{
    if (Blocking()) {
        Note(g_saidKey, "GetKeyState");
        return 0;
    }
    return oKey(key);
}

// Only the game's own import is replaced, so the mod and the overlay keep reading the real keyboard.
template <typename T>
bool PatchImport(const char* dll, const char* function, void* detour, T& original)
{
    auto* base = reinterpret_cast<uint8_t*>(g_gameBegin);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    const auto& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!directory.VirtualAddress)
        return false;
    for (auto* entry = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + directory.VirtualAddress); entry->Name; ++entry) {
        if (_stricmp(reinterpret_cast<const char*>(base + entry->Name), dll) != 0 || !entry->OriginalFirstThunk)
            continue;
        auto* names = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + entry->OriginalFirstThunk);
        auto* slots = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + entry->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++slots) {
            if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal))
                continue;
            const auto* named = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            if (strcmp(reinterpret_cast<const char*>(named->Name), function) != 0)
                continue;
            DWORD protect = 0;
            if (!VirtualProtect(&slots->u1.Function, sizeof(ULONGLONG), PAGE_READWRITE, &protect))
                return false;
            original = reinterpret_cast<T>(slots->u1.Function);
            slots->u1.Function = reinterpret_cast<ULONGLONG>(detour);
            VirtualProtect(&slots->u1.Function, sizeof(ULONGLONG), protect, &protect);
            return true;
        }
    }
    return false;
}

using DeviceStateFn = HRESULT(STDMETHODCALLTYPE*)(IDirectInputDevice8W*, DWORD, LPVOID);
using DeviceDataFn = HRESULT(STDMETHODCALLTYPE*)(IDirectInputDevice8W*, DWORD, LPDIDEVICEOBJECTDATA, LPDWORD, DWORD);
DeviceStateFn oState = nullptr;
DeviceDataFn oData = nullptr;

bool IsKeyboard(IDirectInputDevice8W* device)
{
    DIDEVICEINSTANCEW info {};
    info.dwSize = sizeof(info);
    return SUCCEEDED(device->GetDeviceInfo(&info)) && GET_DIDEVICE_TYPE(info.dwDevType) == DI8DEVTYPE_KEYBOARD;
}

HRESULT STDMETHODCALLTYPE DetourState(IDirectInputDevice8W* device, DWORD size, LPVOID data)
{
    const HRESULT result = oState(device, size, data);
    if (SUCCEEDED(result) && size == 256 && data && Blocking()) {
        Note(g_saidState, "DirectInput device state");
        std::memset(data, 0, size);
    }
    return result;
}

// Still read, so nothing piles up to arrive when the editor closes.
HRESULT STDMETHODCALLTYPE DetourData(IDirectInputDevice8W* device, DWORD objectSize, LPDIDEVICEOBJECTDATA data, LPDWORD count, DWORD flags)
{
    const HRESULT result = oData(device, objectSize, data, count, flags);
    if (SUCCEEDED(result) && count && *count > 0 && Blocking() && IsKeyboard(device)) {
        Note(g_saidData, "DirectInput device data");
        *count = 0;
    }
    return result;
}

template <typename T>
bool Hook(void* target, void* detour, T& original, const char* name)
{
    if (!target || MH_CreateHook(target, detour, reinterpret_cast<void**>(&original)) != MH_OK || MH_EnableHook(target) != MH_OK) {
        mod::Log("Input: couldn't hook %s", name);
        return false;
    }
    return true;
}

} // namespace

void Install()
{
    const scan::Module module = scan::MainModule();
    g_gameBegin = module.Base;
    g_gameEnd = module.Base + module.Size;

    if (!PatchImport("USER32.dll", "GetAsyncKeyState", reinterpret_cast<void*>(&DetourAsync), oAsync))
        mod::Log("Input: couldn't replace the game's GetAsyncKeyState");
    if (!PatchImport("USER32.dll", "GetKeyState", reinterpret_cast<void*>(&DetourKey), oKey))
        mod::Log("Input: couldn't replace the game's GetKeyState");

    // The methods are shared by every keyboard, so a throwaway device shows where they are.
    IDirectInput8W* input = nullptr;
    if (FAILED(DirectInput8Create(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION, IID_IDirectInput8W, reinterpret_cast<void**>(&input), nullptr)) || !input) {
        mod::Log("Input: DirectInput isn't available");
        return;
    }
    IDirectInputDevice8W* keyboard = nullptr;
    if (SUCCEEDED(input->CreateDevice(GUID_SysKeyboard, &keyboard, nullptr)) && keyboard) {
        void** table = *reinterpret_cast<void***>(keyboard);
        Hook(table[9], reinterpret_cast<void*>(&DetourState), oState, "GetDeviceState");
        Hook(table[10], reinterpret_cast<void*>(&DetourData), oData, "GetDeviceData");
        keyboard->Release();
    }
    input->Release();
}

} // namespace inputblock
