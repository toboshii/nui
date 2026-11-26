// pad_shim.cpp - a stand-in for the client's xinputdll.dll, for pad_test.exe: __cdecl functions with the
// shim's underscore names, which call SHIM_TARGET's XInputGetState (pad_fake_nohot.dll, a build that is not
// hot-patchable, as on the machine where route 0 refused). XInputGetStateAlt_ also holds Start, so a test
// can tell which function the client's F slot points at.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#ifndef SHIM_TARGET
#define SHIM_TARGET "pad_fake_nohot.dll"
#endif

typedef DWORD(__stdcall* get_fn)(DWORD, void*);

static get_fn target() {
    static get_fn fn = nullptr;
    if (!fn) {
        HMODULE m = GetModuleHandleA(SHIM_TARGET);
        if (!m) m = LoadLibraryA(SHIM_TARGET);
        if (m) fn = reinterpret_cast<get_fn>(reinterpret_cast<void*>(GetProcAddress(m, "XInputGetState")));
    }
    return fn;
}

extern "C" __declspec(dllexport) DWORD __cdecl XInputGetState_(DWORD index, void* state) {
    get_fn const fn = target();
    return fn ? fn(index, state) : ERROR_DEVICE_NOT_CONNECTED;
}

extern "C" __declspec(dllexport) DWORD __cdecl XInputGetStateAlt_(DWORD index, void* state) {
    DWORD const r = XInputGetState_(index, state);
    if (r == ERROR_SUCCESS && state) static_cast<WORD*>(state)[2] |= 0x0010;     // XINPUT_STATE.Gamepad.wButtons
    return r;
}

extern "C" __declspec(dllexport) DWORD __cdecl XInputSetState_(DWORD, void*) { return ERROR_SUCCESS; }
extern "C" __declspec(dllexport) void __cdecl XInputEnable_(BOOL) {}

BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) { return TRUE; }
