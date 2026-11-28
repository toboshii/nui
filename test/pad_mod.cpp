// pad_mod.cpp - a second module on the PadHook bus, for pad_test.exe and pad_di_test.exe (built as pad_mod_r<REV>.dll with
// -DPADHOOK_REV=<n>, or as pad_mod_r2.dll with -DPADMOD_REV2 from the frozen revision 2 header). It leaves
// in DllMain, as a real module does.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#ifdef PADMOD_REV2
#include "padhook_rev2/PadHook.h"
#else
#include "../src/PadHook/PadHook.h"
#endif

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
#ifndef PADMOD_REV2
extern "C" __declspec(dllexport) void* __cdecl mod_di_dispatch() { return reinterpret_cast<void*>(&padhook_detail::di_dispatch); }
#endif

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_DETACH && g_slot >= 0) padhook_unregister(padhook_detail::cached_bus(), g_slot, false);
    return TRUE;
}
