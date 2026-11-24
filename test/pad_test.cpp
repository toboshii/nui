// pad_test.cpp - PadHook.h against a stand-in xinput1_3.dll (pad_fake_xinput.cpp), with the exact
// entry shape of the real one, in a real process: the patch bytes, pass-through, recording, the mask
// rules, packet renumbering, the heartbeat, a second module at a higher revision taking the dispatch
// and handing it back from its DllMain, and the frozen layout. This exe is module A.
//
//   pad_test.exe main <layout_padhook.txt>   (PADHOOK_DLL = pad_fake_xinput.dll)
//   pad_test.exe foreign                     another tool's hot-patch already on the entry: refused
//   pad_test_nohot.exe nohot                 (PADHOOK_DLL = pad_fake_nohot.dll) not hot-patchable: refused
#include "../src/PadHook/PadHook.h"

#include "pad_layout.hpp"

#include <cstdio>
#include <cstring>
#include <string>

namespace {

int g_pass = 0, g_fail = 0;
void check(char const* name, bool ok, char const* detail = "") {
    if (ok) ++g_pass;
    else { ++g_fail; std::printf("FAIL %s %s\n", name, detail); }
}

typedef void(__cdecl* set_fn)(int, int, int, int, int, int, int);
typedef int(__cdecl* join_fn)();
typedef void(__cdecl* cap_fn)(int);
typedef void*(__cdecl* ptr_fn)();

padhook_get_fn g_get = nullptr;
set_fn g_set = nullptr;

constexpr unsigned A = 0x1000, B = 0x2000, X = 0x4000, Y = 0x8000;

PadXState read(DWORD index = 0, DWORD* r = nullptr) {
    PadXState s {};
    DWORD const rr = g_get(index, &s);
    if (r) *r = rr;
    return s;
}
PadXState press(unsigned buttons, int lt = 0, int rt = 0, int lx = 0) {
    g_set(static_cast<int>(buttons), lt, rt, lx, 0, 0, 0);
    return read();
}

bool load_fake(char const* dll) {
    HMODULE fx = LoadLibraryA(dll);
    if (!fx) return false;
    g_get = reinterpret_cast<padhook_get_fn>(reinterpret_cast<void*>(GetProcAddress(fx, "XInputGetState")));
    g_set = reinterpret_cast<set_fn>(reinterpret_cast<void*>(GetProcAddress(fx, "fake_set")));
    return g_get && g_set;
}

int run_main(char const* layout_path) {
    PadBus* bus = padhook_attach();
    check("attach", bus != nullptr);
    if (!bus) return 1;

    // not loaded yet: retry, and nothing installed
    check("not loaded: retry", padhook_join(bus, "test") == PADHOOK_JOIN_RETRY, bus->status);
    check("not loaded: says so", std::strstr(bus->status, "not loaded") != nullptr, bus->status);
    check("simulated missing: retry", padhook_join(bus, "test", true) == PADHOOK_JOIN_RETRY);
    check("not loaded: waiting", padhook_state(bus) == PADHOOK_WAIT);

    check("fake loads", load_fake(PADHOOK_DLL));
    if (!g_get) return 1;
    auto const* site = reinterpret_cast<unsigned char const*>(reinterpret_cast<void*>(g_get));
    check("fake is hot-patchable", site[0] == 0x8B && site[1] == 0xFF && site[-1] == 0x90);
    PadXState raw = press(A);
    check("unhooked read", raw.buttons == A);

    int const slot = padhook_join(bus, "test");
    check("join", slot >= 0, bus->status);
    check("patched entry EB F9", site[0] == 0xEB && site[1] == 0xF9);
    check("patched padding E9", site[-5] == 0xE9);
    check("live", padhook_state(bus) == PADHOOK_LIVE);
    check("drives", bus->owner_slot == slot && *bus->dispatch_ptr == &padhook_detail::dispatch);
    check("one install", bus->installs == 1);

    // pass-through and recording
    LONG const calls0 = bus->calls;
    PadXState s = press(A, 0, 0, 1234);
    check("pass-through: buttons", s.buttons == A);
    check("pass-through: stick", s.lx == 1234);
    check("through the dispatcher", bus->calls == calls0 + 1);
    PadState ps {};
    check("read back", padhook_read(bus, 0, &ps));
    check("recorded buttons", ps.buttons == A && ps.connected == 1 && ps.source == PADHOOK_SRC_XINPUT && ps.lx == 1234);
    check("last index", bus->last_index == 0);
    unsigned long const seq0 = ps.seq;
    read();
    padhook_read(bus, 0, &ps);
    check("same state: same seq", ps.seq == seq0);
    press(A | B);
    padhook_read(bus, 0, &ps);
    check("new state: new seq", ps.seq != seq0 && ps.buttons == (A | B));
    DWORD r = 0;
    read(1, &r);
    check("index 1 not connected", r == ERROR_DEVICE_NOT_CONNECTED);
    padhook_read(bus, 1, &ps);
    check("index 1 recorded as not connected", ps.connected == 0);
    check("last index stays the connected one", bus->last_index == 0);
    press(0);
    PadXState const trig = press(0, 200, 0);
    padhook_read(bus, 0, &ps);
    check("a trigger is a held bit", (ps.buttons & PADHOOK_LT) && trig.lt == 200);
    press(0);

    // capture: B held before it reaches the game until released; new presses are hidden
    press(B);
    padhook_capture(bus, slot, true);
    check("holding", padhook_holding(bus, slot));
    s = press(B | A, 200, 0, 20000);
    check("held before: still the game's", (s.buttons & B) != 0);
    check("new press: hidden", (s.buttons & A) == 0);
    check("stick centred", s.lx == 0);
    check("trigger released", s.lt == 0);
    padhook_read(bus, 0, &ps);
    check("raw still recorded", ps.buttons == (A | B | PADHOOK_LT) && ps.lx == 20000);
    s = press(A);
    check("B released", s.buttons == 0);
    s = press(A | B);
    check("B pressed again while held: hidden", s.buttons == 0);
    // renumbering: what the game sees did not change, so neither does its packet
    DWORD const p1 = s.packet;
    s = press(A | B | X);
    check("hidden change: same packet", s.packet == p1 && s.buttons == 0);
    // give the pad back with A, B, X held: hidden until each is released
    padhook_capture(bus, slot, false);
    s = press(A | B | X);
    check("given back: held presses stay hidden", s.buttons == 0);
    s = press(A | 0, 0, 0, 500);
    check("given back: sticks are the game's again", s.lx == 500);
    check("given back: still hidden until released", s.buttons == 0);
    s = press(0);
    s = press(A);
    check("released and pressed again: the game's", s.buttons == A);
    check("shown again: new packet", s.packet != p1);
    press(0);

    // the heartbeat: a capture not renewed lapses by itself
    padhook_capture(bus, slot, true);
    s = press(Y);
    check("captured: Y hidden", s.buttons == 0);
    press(0);
    Sleep(PADHOOK_HEARTBEAT_MS + 150);
    s = press(A);
    check("lapsed: A reaches the game", s.buttons == A);
    check("lapse counted", bus->lapses >= 1);
    check("lapsed: not holding", !padhook_holding(bus, slot));
    press(0);
    padhook_capture(bus, slot, true);
    for (int i = 0; i < 4; ++i) { Sleep(PADHOOK_HEARTBEAT_MS / 2); padhook_alive(bus, slot); }
    s = press(A);
    check("renewed: still holding", s.buttons == 0 && padhook_holding(bus, slot));
    padhook_capture(bus, slot, false);
    press(0);

    // a second module at revision 2 takes the dispatch; its DllMain hands it back
    HMODULE m2 = LoadLibraryA("pad_mod_r2.dll");
    check("module r2 loads", m2 != nullptr);
    if (m2) {
        auto mjoin = reinterpret_cast<join_fn>(reinterpret_cast<void*>(GetProcAddress(m2, "mod_join")));
        auto mcap = reinterpret_cast<cap_fn>(reinterpret_cast<void*>(GetProcAddress(m2, "mod_capture")));
        auto mdis = reinterpret_cast<ptr_fn>(reinterpret_cast<void*>(GetProcAddress(m2, "mod_dispatch")));
        int const s2 = mjoin ? mjoin() : -1;
        check("r2 joins", s2 >= 0 && s2 != slot);
        check("r2 drives", bus->owner_slot == s2 && reinterpret_cast<void*>(*bus->dispatch_ptr) == mdis());
        check("no second install", bus->installs == 1);
        s = press(A);
        check("r2 dispatches", s.buttons == A);
        mcap(1);
        s = press(A | B);
        check("r2's capture hides for everyone", (s.buttons & B) == 0 && (s.buttons & A) != 0);
        press(0);
        FreeLibrary(m2);
        check("r2 left: the dispatch is back with A", bus->owner_slot == slot && *bus->dispatch_ptr == &padhook_detail::dispatch);
        s = press(A);
        check("after r2 unloaded: reads still work", s.buttons == A, "");
        check("r2's capture went with it", s.buttons == A);
        press(0);
    }

    // leaving: a pass-through, the bytes stay
    padhook_unregister(bus, slot, true);
    check("left: nobody drives", *bus->dispatch_ptr == nullptr && bus->owner_slot == -1);
    LONG const c0 = bus->calls;
    s = press(X, 0, 0, 77);
    check("pass-through with nobody", s.buttons == X && s.lx == 77 && bus->calls == c0);
    check("patch kept", site[0] == 0xEB && site[1] == 0xF9);
    int const again = padhook_join(bus, "test");
    check("rejoin", again >= 0 && bus->installs == 1);
    padhook_unregister(bus, again, true);

    // the frozen layout
    char text[8192];
    padhook_layout_text(text, sizeof(text));
    std::string want;
    if (FILE* f = std::fopen(layout_path, "rb")) {
        char buf[4096];
        std::size_t n;
        while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) want.append(buf, n);
        std::fclose(f);
    }
    std::string got;
    for (std::size_t at = 0; at < want.size();) {       // the file's non-comment lines
        std::size_t const end = want.find('\n', at);
        std::string line = want.substr(at, end == std::string::npos ? std::string::npos : end - at);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty() && line[0] != '#') got += line + "\n";
        at = end == std::string::npos ? want.size() : end + 1;
    }
    check("layout_padhook.txt", got == text, "");
    if (got != text) std::printf("--- header layout:\n%s", text);
    return 0;
}

int run_foreign() {
    check("fake loads", load_fake(PADHOOK_DLL));
    if (!g_get) return 1;
    // another tool hot-patched it first
    auto* site = reinterpret_cast<unsigned char*>(reinterpret_cast<void*>(g_get));
    DWORD old = 0;
    VirtualProtect(site - 5, 7, PAGE_EXECUTE_READWRITE, &old);
    unsigned char const pad[5] = {0xE9, 0, 0, 0, 0};       // jmp +0: falls into the entry, a harmless detour
    std::memcpy(site - 5, pad, 5);
    VirtualProtect(site - 5, 7, old, &old);
    PadBus* bus = padhook_attach();
    check("foreign: blocked", padhook_join(bus, "test") == PADHOOK_JOIN_BLOCKED, bus->status);
    check("foreign: says so", std::strstr(bus->status, "already hooked") != nullptr, bus->status);
    check("foreign: entry untouched", site[0] == 0x8B && site[1] == 0xFF);
    check("foreign: never retried", padhook_join(bus, "test") == PADHOOK_JOIN_BLOCKED);
    return 0;
}

int run_nohot() {
    check("fake loads", load_fake(PADHOOK_DLL));
    if (!g_get) return 1;
    auto const* site = reinterpret_cast<unsigned char const*>(reinterpret_cast<void*>(g_get));
    unsigned char before[7];
    std::memcpy(before, site - 5, 7);
    PadBus* bus = padhook_attach();
    check("nohot: blocked", padhook_join(bus, "test") == PADHOOK_JOIN_BLOCKED, bus->status);
    check("nohot: says so", std::strstr(bus->status, "not hot-patchable") != nullptr, bus->status);
    check("nohot: bytes untouched", std::memcmp(before, site - 5, 7) == 0);
    check("nohot: state blocked", padhook_state(bus) == PADHOOK_BLOCKED);
    PadXState const s = press(A);
    check("nohot: the game still reads", s.buttons == A);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::string const mode = argc > 1 ? argv[1] : "main";
    if (mode == "main") run_main(argc > 2 ? argv[2] : "layout_padhook.txt");
    else if (mode == "foreign") run_foreign();
    else if (mode == "nohot") run_nohot();
    else { std::printf("unknown mode %s\n", mode.c_str()); return 2; }
    std::printf("pad_test %s: %d passed, %d failed\n", mode.c_str(), g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
