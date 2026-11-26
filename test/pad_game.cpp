// pad_game.cpp - a stand-in for the client's FFXiMain.dll, for pad_test.exe (built as game/FFXiMain.dll).
// It holds the client's XInput wrappers with the exact bytes of the 2026-11-02 build, apart from the slot
// addresses, which point at this DLL's own four slots (H, F, S, E). SetState's wrapper comes first here, so
// a scan that takes the first match patches the wrong one (the client puts GetState first). Built with
// -DGAME_NOMATCH as game_nomatch/FFXiMain.dll, the GetState wrapper pushes its arguments with other bytes
// that do the same thing, so the signature finds no GetState wrapper (a client update).
//
// The exports do what the client does: game_setup loads xinputdll.dll and writes all four slots (at input
// init and on each re-enumeration), game_unload frees it and zeros only H, and game_read calls the wrapper
// as the game does. game_point puts another of the shim's functions into F (a reload that moved it).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

extern "C" {
__attribute__((used)) volatile DWORD g_slots[4] = {0, 0, 0, 0};   // .data: H, F, S, E
DWORD __stdcall getstate_wrapper(DWORD index, void* state);
}

#define WRAP_HEAD(h, f) ".byte 0xA1\n.long " h "\n.byte 0x85,0xC0,0x74,0x1B,0xA1\n.long " f "\n.byte 0x85,0xC0,0x74,0x12\n"
#define WRAP_TAIL ".byte 0xFF,0xD0,0x83,0xC4,0x08,0xC2,0x08,0x00,0xB8,0x01,0x00,0x00,0x00,0xC2,0x08,0x00,0x90,0x90,0x90,0x90\n"
#define WRAP_PUSH ".byte 0x8B,0x4C,0x24,0x08,0x8B,0x54,0x24,0x04,0x51,0x52\n"
#ifndef GAME_NOMATCH
#define GET_PUSH WRAP_PUSH
#else
#define GET_PUSH ".byte 0xFF,0x74,0x24,0x08,0xFF,0x74,0x24,0x08,0x90,0x90\n"   // push [esp+8] twice: the same, other bytes
#endif

asm(".text\n.balign 16\n"
    "_setstate_wrapper@8:\n" WRAP_HEAD("_g_slots", "_g_slots+8") WRAP_PUSH WRAP_TAIL
    ".globl _getstate_wrapper@8\n"
    "_getstate_wrapper@8:\n" WRAP_HEAD("_g_slots", "_g_slots+4") GET_PUSH WRAP_TAIL
    "_enable_wrapper@4:\n"
    ".byte 0xA1\n.long _g_slots\n.byte 0x85,0xC0,0x74,0x11,0xA1\n.long _g_slots+12\n.byte 0x85,0xC0,0x74,0x08\n"
    ".byte 0xFF,0x74,0x24,0x04,0xFF,0xD0,0x59,0x90,0xC2,0x04,0x00,0xC2,0x04,0x00\n");

extern "C" __declspec(dllexport) DWORD __cdecl game_read(DWORD index, void* state) { return getstate_wrapper(index, state); }

extern "C" __declspec(dllexport) int __cdecl game_setup() {
    HMODULE shim = LoadLibraryA("xinputdll.dll");
    g_slots[0] = reinterpret_cast<DWORD>(shim);
    if (!shim) return 0;
    g_slots[1] = reinterpret_cast<DWORD>(reinterpret_cast<void*>(GetProcAddress(shim, "XInputGetState_")));
    g_slots[2] = reinterpret_cast<DWORD>(reinterpret_cast<void*>(GetProcAddress(shim, "XInputSetState_")));
    g_slots[3] = reinterpret_cast<DWORD>(reinterpret_cast<void*>(GetProcAddress(shim, "XInputEnable_")));
    return 1;
}

extern "C" __declspec(dllexport) void __cdecl game_unload() {
    HMODULE const shim = reinterpret_cast<HMODULE>(g_slots[0]);
    if (shim) FreeLibrary(shim);
    g_slots[0] = 0;
}

extern "C" __declspec(dllexport) void __cdecl game_point(int alt) {
    HMODULE const shim = reinterpret_cast<HMODULE>(g_slots[0]);
    if (shim) g_slots[1] = reinterpret_cast<DWORD>(reinterpret_cast<void*>(GetProcAddress(shim, alt ? "XInputGetStateAlt_" : "XInputGetState_")));
}

extern "C" __declspec(dllexport) void* __cdecl game_wrapper() { return reinterpret_cast<void*>(&getstate_wrapper); }
extern "C" __declspec(dllexport) volatile DWORD* __cdecl game_slots() { return g_slots; }

BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) { return TRUE; }
