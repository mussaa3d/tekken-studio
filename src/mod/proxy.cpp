// dinput8.dll forwarders. Tekken 8 imports DINPUT8.dll, so a dinput8.dll next to the exe is how the
// mod gets loaded (the same slot opendojo and other Tekken 8 mods use). Everything here just calls
// the real System32 DLL.
#include <windows.h>
#include <unknwn.h>

#pragma comment(linker, "/export:DirectInput8Create")
#pragma comment(linker, "/export:DllCanUnloadNow,PRIVATE")
#pragma comment(linker, "/export:DllGetClassObject,PRIVATE")
#pragma comment(linker, "/export:DllRegisterServer,PRIVATE")
#pragma comment(linker, "/export:DllUnregisterServer,PRIVATE")
#pragma comment(linker, "/export:GetdfDIJoystick")

namespace {
HMODULE g_real = nullptr;
template <typename T>
T Export(const char* name)
{
    return g_real ? reinterpret_cast<T>(GetProcAddress(g_real, name)) : nullptr;
}
HRESULT(WINAPI* p_Create)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN) = nullptr;
HRESULT(WINAPI* p_CanUnload)() = nullptr;
HRESULT(WINAPI* p_GetClass)(REFCLSID, REFIID, LPVOID*) = nullptr;
HRESULT(WINAPI* p_Register)() = nullptr;
HRESULT(WINAPI* p_Unregister)() = nullptr;
void*(WINAPI* p_Joystick)() = nullptr;
} // namespace

void LoadRealDinput8()
{
    if (g_real)
        return;
    wchar_t path[MAX_PATH];
    const UINT n = GetSystemDirectoryW(path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH - 16)
        return;
    wcscat_s(path, L"\\dinput8.dll");
    g_real = LoadLibraryW(path);
    p_Create = Export<decltype(p_Create)>("DirectInput8Create");
    p_CanUnload = Export<decltype(p_CanUnload)>("DllCanUnloadNow");
    p_GetClass = Export<decltype(p_GetClass)>("DllGetClassObject");
    p_Register = Export<decltype(p_Register)>("DllRegisterServer");
    p_Unregister = Export<decltype(p_Unregister)>("DllUnregisterServer");
    p_Joystick = Export<decltype(p_Joystick)>("GetdfDIJoystick");
}

extern "C" HRESULT WINAPI DirectInput8Create(HINSTANCE h, DWORD v, REFIID r, LPVOID* p, LPUNKNOWN u)
{
    return p_Create ? p_Create(h, v, r, p, u) : E_FAIL;
}
extern "C" HRESULT WINAPI DllCanUnloadNow() { return p_CanUnload ? p_CanUnload() : S_OK; }
extern "C" HRESULT WINAPI DllGetClassObject(REFCLSID c, REFIID r, LPVOID* p)
{
    return p_GetClass ? p_GetClass(c, r, p) : E_FAIL;
}
extern "C" HRESULT WINAPI DllRegisterServer() { return p_Register ? p_Register() : S_OK; }
extern "C" HRESULT WINAPI DllUnregisterServer() { return p_Unregister ? p_Unregister() : S_OK; }
extern "C" void* WINAPI GetdfDIJoystick() { return p_Joystick ? p_Joystick() : nullptr; }
