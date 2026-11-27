// key_mod.cpp - a second module on the KeyHook bus, for key_test.exe (built as key_mod_r2.dll with -DKEYHOOK_REV=2).
// It leaves in DllMain, as a real module does.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "../src/KeyHook/KeyHook.h"

static int g_slot = -1;

extern "C" __declspec(dllexport) int __cdecl mod_join() {
    g_slot = keyhook_join(keyhook_attach(), "r2");
    return g_slot;
}
extern "C" __declspec(dllexport) void __cdecl mod_hide(int code) {
    unsigned long mask[KEYHOOK_WORDS] = {0, 0, 0, 0, 0, 0, 0, 0};
    if (code >= 0 && code < KEYHOOK_KEYS) mask[code >> 5] = 1ul << (code & 31);
    keyhook_hide(keyhook_attach(), g_slot, code >= 0 ? mask : nullptr);
}
extern "C" __declspec(dllexport) void* __cdecl mod_dispatch() { return reinterpret_cast<void*>(&keyhook_detail::dispatch); }

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_DETACH && g_slot >= 0) keyhook_unregister(keyhook_detail::cached_bus(), g_slot, false);
    return TRUE;
}
