// pad_fake_xinput.cpp - a stand-in for xinput1_3.dll, for pad_test.exe. Its XInputGetState has the
// shape of the real one's entry: five bytes of padding (90), then 8B FF 55 8B EC (mov edi, edi; push
// ebp; mov ebp, esp). Built as pad_fake_xinput.dll; with -DFAKE_NOHOT as pad_fake_nohot.dll, whose
// entry is not hot-patchable (55 8B EC, no mov edi, edi), which PadHook must refuse to patch.
//
// The state it returns is set by fake_set (index 0 only; every other index is not connected).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

struct FakeState {
    DWORD packet;
    WORD buttons;
    BYTE lt, rt;
    SHORT lx, ly, rx, ry;
};

static FakeState g_state = {};
static volatile LONG g_calls = 0;

extern "C" DWORD __stdcall fake_impl(DWORD index, FakeState* s) {
    InterlockedIncrement(&g_calls);
    if (index != 0) return ERROR_DEVICE_NOT_CONNECTED;
    if (s) *s = g_state;
    return ERROR_SUCCESS;
}

#ifndef FAKE_NOHOT
asm(".text\n.globl _XInputGetState@8\n.balign 16\n.byte 0x90,0x90,0x90,0x90,0x90\n_XInputGetState@8:\n"
    ".byte 0x8B,0xFF\n.byte 0x55\n.byte 0x8B,0xEC\n.byte 0x5D\njmp _fake_impl@8\n");
#else
asm(".text\n.globl _XInputGetState@8\n.balign 16\n.byte 0x90,0x90,0x90,0x90,0x90\n_XInputGetState@8:\n"
    ".byte 0x55\n.byte 0x8B,0xEC\n.byte 0x5D\njmp _fake_impl@8\n");
#endif
asm(".section .drectve\n.ascii \" -export:\\\"XInputGetState@8\\\"\"\n.text\n");

// every call is a new packet, as a real pad's state change
extern "C" __declspec(dllexport) void __cdecl fake_set(int buttons, int lt, int rt, int lx, int ly, int rx, int ry) {
    g_state.packet += 1;
    g_state.buttons = static_cast<WORD>(buttons);
    g_state.lt = static_cast<BYTE>(lt);
    g_state.rt = static_cast<BYTE>(rt);
    g_state.lx = static_cast<SHORT>(lx);
    g_state.ly = static_cast<SHORT>(ly);
    g_state.rx = static_cast<SHORT>(rx);
    g_state.ry = static_cast<SHORT>(ry);
}

extern "C" __declspec(dllexport) LONG __cdecl fake_calls() { return g_calls; }

BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) { return TRUE; }
