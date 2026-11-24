// pad_mod.cpp - a second module on the PadHook bus, for pad_test.exe (built as pad_mod_r<REV>.dll with
// -DPADHOOK_REV=<n>). It leaves in DllMain, as a real module does.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "../src/PadHook/PadHook.h"

#ifndef PADMOD_NAME
#define PADMOD_NAME "mod"
#endif

static int g_slot = -1;

extern "C" __declspec(dllexport) int __cdecl mod_join() {
    g_slot = padhook_join(padhook_attach(), PADMOD_NAME);
    return g_slot;
}
extern "C" __declspec(dllexport) void __cdecl mod_capture(int on) { padhook_capture(padhook_attach(), g_slot, on != 0); }
extern "C" __declspec(dllexport) void* __cdecl mod_dispatch() { return reinterpret_cast<void*>(&padhook_detail::dispatch); }

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_DETACH && g_slot >= 0) padhook_unregister(padhook_detail::cached_bus(), g_slot, false);
    return TRUE;
}
