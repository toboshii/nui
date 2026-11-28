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
// With -DGAME_DI it also holds the client's two DirectInput pad reads (PadHook rev 4, below).
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

#ifdef GAME_DI
// PadHook rev 4: the client's input update, cut to its two DirectInput pad reads (2026-11-02, 1000616B and 100069AC),
// byte for byte at the client's 8-byte alignment (the cmp at 8n+2, the store at 8n+3). Only the bytes that name addresses
// differ: they point into this DLL. game_di_update(input) runs it as the client does; recorders after the cmp and after
// the store keep the flags, eax and the client's flag for pad_di_test.exe. Built with -DGAME_DI_NOMATCH, the event read
// calls another method, so the signature finds no event read (a client update).
extern "C" {
__attribute__((used)) volatile DWORD g_di_rec[6] = {0, 0, 0, 0, 0, 0};   // flags after the cmp, eax, flag, failed, ebp, edi
__attribute__((used)) volatile unsigned char g_di_gamepad = 1;
__attribute__((used)) volatile DWORD g_di_sink[2] = {0, 0};
}
#ifndef GAME_DI_NOMATCH
#define DI_READ ".byte 0xFF,0x52,0x28\n"                      // call [edx+0x28]: GetDeviceData
#else
#define DI_READ ".byte 0xFF,0x52,0x2C\n"                      // call [edx+0x2C]: another method
#endif
asm(".text\n.balign 16\n"
    ".globl _game_di_update\n"
    "_game_di_update:\n"
    "pushl %ebp\npushl %ebx\npushl %esi\npushl %edi\n"
    "subl $0x60, %esp\n"
    "movl 0x74(%esp), %ebx\n"
    "movl 0x90(%ebx), %eax\n"
    "testl %eax, %eax\n"
    "jz 3f\n"
    ".balign 8, 0x90\n.byte 0x90,0x90,0x90\n"
    ".globl _game_di_site_a\n_game_di_site_a:\n"
    ".byte 0x8A,0x8B,0x98,0x0C,0x00,0x00,0x84,0xC9,0x74,0x06,0x8B,0x08,0x50,0xFF,0x51,0x64,0x8B,0xCB,0xE8\n"
    ".long 8f - . - 4\n"
    ".byte 0x8B,0x83,0x90,0x00,0x00,0x00,0x8D,0x8B,0x44,0x0A,0x00,0x00,0x51,0x68,0x10,0x01,0x00,0x00\n"
    ".byte 0x8B,0x10,0x50,0xFF,0x52,0x24,0x3D,0xAA,0x00,0x07,0x80\n"
    "pushfl\npopl _g_di_rec\nmovl %eax, _g_di_rec+4\n"
    "3:\n"
    "movb $0, 0x13(%esp)\n"
    ".balign 8, 0x90\n.byte 0x90,0x90,0x90,0x90\n"
    ".globl _game_di_site_b\n_game_di_site_b:\n"
    ".byte 0x8D,0xAB,0x94,0x0C,0x00,0x00,0xC7,0x45,0x00,0x10,0x00,0x00,0x00,0xA0\n"
    ".long _g_di_gamepad\n"
    ".byte 0x84,0xC0,0x0F,0x84\n.long 9f - . - 4\n"
    ".byte 0x8B,0x83,0x90,0x00,0x00,0x00,0x85,0xC0,0x0F,0x84\n.long 9f - . - 4\n"
    ".byte 0x8B,0x10,0x8D,0xBB,0x54,0x0B,0x00,0x00,0x6A,0x00,0x55,0x57,0x6A,0x14,0x50\n"
    DI_READ
    ".byte 0x85,0xC0,0x74,0x15,0xB9,0x50,0x00,0x00,0x00,0x33,0xC0,0xC7,0x45,0x00,0x00,0x00,0x00,0x00,0xF3,0xAB,0xE9\n"
    ".long 7f - . - 4\n"
    ".byte 0xA1\n.long _g_di_sink\n"
    ".byte 0xC6,0x05\n.long _g_di_sink+4\n.byte 0x00\n"
    ".byte 0xC6,0x44,0x24,0x13,0x01\n"
    "movzbl 0x13(%esp), %eax\nmovl %eax, _g_di_rec+8\n"
    "movl %ebp, _g_di_rec+16\nmovl %edi, _g_di_rec+20\n"
    "jmp 9f\n"
    "7:\nmovl $1, _g_di_rec+12\n"
    "9:\n"
    "addl $0x60, %esp\npopl %edi\npopl %esi\npopl %ebx\npopl %ebp\nret\n"
    "8:\nret\n");
extern "C" void game_di_update(void* input);
extern "C" char game_di_site_a[], game_di_site_b[];
extern "C" __declspec(dllexport) void __cdecl game_di(void* input) { game_di_update(input); }
extern "C" __declspec(dllexport) void* __cdecl game_di_fn() { return reinterpret_cast<void*>(&game_di_update); }
extern "C" __declspec(dllexport) void* __cdecl game_di_sites(int which) { return which ? game_di_site_b : game_di_site_a; }
extern "C" __declspec(dllexport) volatile DWORD* __cdecl game_di_rec() { return g_di_rec; }
extern "C" __declspec(dllexport) void __cdecl game_di_gamepad(int on) { g_di_gamepad = on ? 1 : 0; }
#endif

BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) { return TRUE; }
